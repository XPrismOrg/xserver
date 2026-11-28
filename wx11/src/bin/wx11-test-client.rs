// SPDX-License-Identifier: MIT
//
// A minimal Wayland client for WX11's tests: one xdg_toplevel filled with
// a colour, from an shm buffer. Prints what happens on stdout, one line
// each: "drawn W H" once the compositor shows a frame, "configure W H"
// for a size asked for, "closed" when asked to close (then exits), and the
// input it gets: "keymap", "keyboard enter|leave", "key CODE pressed|released"
// (evdev codes), "modifiers DEPRESSED", "pointer enter X Y", "pointer leave",
// "motion X Y", "button CODE pressed|released", "axis vertical|horizontal V";
// "output W H" and "output size MMW MMH" for the wl_output, "surface
// enter output" when its window is on it.
//
// With --popup WxH+X+Y it opens a popup at X, Y of the window once that is
// drawn, or, with --grab, as a grabbing menu when the window is clicked:
// "popup configure X Y W H", "popup drawn", "popup done"; input events on
// the popup say "popup" ("pointer enter popup X Y", "keyboard enter popup").
//
// --security-context APP_ID acts as a sandbox would: it registers a
// wp_security_context_v1 listener for APP_ID, then runs through it.
//
// --alpha draws an argb8888 buffer; --color then takes AARRGGBB
// (premultiplied).
//
// --copy TEXT puts TEXT on the clipboard once the window has the keyboard
// ("copied"; "sent" when someone pasted it); --paste prints every clipboard
// text it is offered ("pasted TEXT"). With --primary both use the primary
// selection (zwp_primary_selection_v1) instead of the clipboard.
// --mime TYPE copies and pastes TYPE instead of UTF-8 text; a paste that is
// not text prints "pasted-bin LENGTH SUM" (SUM: the bytes added up).
// --copy-bytes N copies N bytes of a pattern (byte i is i * 7 % 251).
// --paste also prints each type it is offered ("offer TYPE").
// --drop takes drags dropped on the window in the --mime type (UTF-8 text
// by default): "drag enter TYPES" (the types, comma-separated), "drag
// leave", "dropped TEXT".
// --paste-late MS waits MS milliseconds before reading a paste and then
// prints only its length ("pasted-late N"); --copy-stall never writes
// what it is asked to send, nor closes the pipe ("send stalled").
//
//   wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH]
//                    [--popup WxH+X+Y [--popup-color RRGGBB] [--grab] [--popup-slide]]
//                    [--copy TEXT | --copy-bytes N [--copy-stall]]
//                    [--paste [--paste-late MS]] [--drop] [--mime TYPE]
//                    [--primary] [--alpha]
//                    [--security-context APP_ID]

use std::fs::File;
use std::io::Write;
use std::os::fd::AsFd;

use wayland_client::protocol::{
    wl_buffer, wl_callback, wl_compositor, wl_region, wl_subcompositor, wl_subsurface, wl_data_device, wl_data_device_manager, wl_data_offer,
    wl_data_source, wl_keyboard, wl_output, wl_pointer, wl_registry, wl_seat, wl_shm, wl_shm_pool,
    wl_surface,
};
use wayland_client::{
    delegate_noop, event_created_child, Connection, Dispatch, QueueHandle, WEnum,
};
use wayland_protocols::wp::primary_selection::zv1::client::{
    zwp_primary_selection_device_manager_v1, zwp_primary_selection_device_v1,
    zwp_primary_selection_offer_v1, zwp_primary_selection_source_v1,
};
use wayland_protocols::wp::security_context::v1::client::{
    wp_security_context_manager_v1, wp_security_context_v1,
};
use wayland_protocols::xdg::shell::client::{
    xdg_popup, xdg_positioner, xdg_surface, xdg_toplevel, xdg_wm_base,
};

/// --popup: size, position in the window, colour, grab
struct PopupSpec {
    size: (i32, i32),
    at: (i32, i32),
    color: u32,
    grab: bool,
    /// may be slid to stay on the screen (--popup-slide)
    slide: bool,
}

/// --subsurface: size, place in the window, colour, synchronized or not,
/// and the colour it changes to on its own after the first frame
struct SubSpec {
    size: (i32, i32),
    at: (i32, i32),
    color: u32,
    desync: bool,
    recolor: Option<u32>,
    input_none: bool,
}

struct App {
    subcompositor: Option<wl_subcompositor::WlSubcompositor>,
    sub_spec: Option<SubSpec>,
    sub: Option<(wl_surface::WlSurface, wl_subsurface::WlSubsurface)>,
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    wm_base: Option<xdg_wm_base::XdgWmBase>,
    seat: Option<wl_seat::WlSeat>,
    surface: Option<wl_surface::WlSurface>,
    xdg_surface: Option<xdg_surface::XdgSurface>,
    size: (i32, i32),
    asked: Option<(i32, i32)>,
    color: u32,
    exit: bool,
    popup_spec: Option<PopupSpec>,
    popup: Option<(
        wl_surface::WlSurface,
        xdg_surface::XdgSurface,
        xdg_popup::XdgPopup,
    )>,
    popup_opened: bool,
    data_manager: Option<wl_data_device_manager::WlDataDeviceManager>,
    data_device: Option<wl_data_device::WlDataDevice>,
    copy: Option<String>,
    paste: bool,
    /// --drop
    drop_target: bool,
    /// the types of the newest data offer
    offer_mimes: Vec<String>,
    drag_offer: Option<wl_data_offer::WlDataOffer>,
    alpha: bool,
    primary: bool,
    primary_manager:
        Option<zwp_primary_selection_device_manager_v1::ZwpPrimarySelectionDeviceManagerV1>,
    primary_device: Option<zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1>,
}

const TEXT: &str = "text/plain;charset=utf-8";

/// --mime: the type copied and pasted
static MIME: std::sync::OnceLock<String> = std::sync::OnceLock::new();
fn mime() -> String {
    MIME.get().cloned().unwrap_or_else(|| TEXT.to_string())
}

/// --copy-bytes N: the pattern copied
fn pattern(n: usize) -> Vec<u8> {
    (0..n).map(|i| (i * 7 % 251) as u8).collect()
}
static COPY_BYTES: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// --paste-late: milliseconds before reading a paste, 0 for at once
static PASTE_LATE_MS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// --copy-stall
static COPY_STALL: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// a source's Send: write the text, or with --copy-stall keep the pipe
/// open without writing
fn send_copy(fd: std::os::fd::OwnedFd, text: &str) {
    if COPY_STALL.load(std::sync::atomic::Ordering::Relaxed) {
        std::mem::forget(fd);
        say("send stalled".into());
        return;
    }
    let n = COPY_BYTES.load(std::sync::atomic::Ordering::Relaxed);
    let data = if n > 0 {
        pattern(n)
    } else {
        text.as_bytes().to_vec()
    };
    // in a thread: a large copy fills the pipe before the reader drains it
    std::thread::spawn(move || {
        let _ = File::from(fd).write_all(&data);
        say("sent".into());
    });
}

fn say(line: String) {
    let mut out = std::io::stdout();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

impl App {
    fn draw(&mut self, qh: &QueueHandle<App>) {
        let surface = self.surface.clone().unwrap();
        if let (Some(spec), None) = (&self.sub_spec, &self.sub) {
            let sc = self.subcompositor.as_ref().expect("no wl_subcompositor");
            let child = self.compositor.as_ref().unwrap().create_surface(qh, ());
            let sub = sc.get_subsurface(&child, &surface, qh, ());
            sub.set_position(spec.at.0, spec.at.1);
            if spec.desync {
                sub.set_desync();
            }
            if spec.input_none {
                let region = self.compositor.as_ref().unwrap().create_region(qh, ());
                child.set_input_region(Some(&region));
                region.destroy();
            }
            // synchronized: shown with the parent's next commit
            self.fill(&child, spec.size, spec.color, qh);
            child.commit();
            self.sub = Some((child, sub));
        }
        self.paint(&surface, self.size, self.color, false, qh);
    }

    /// attach a buffer of color to surface (no commit)
    fn fill(&self, surface: &wl_surface::WlSurface, (w, h): (i32, i32), color: u32, qh: &QueueHandle<App>) {
        let stride = w * 4;
        let len = (stride * h) as usize;
        let file = shm_file(len);
        let mut data = Vec::with_capacity(len);
        for _ in 0..(w * h) {
            data.extend_from_slice(&color.to_le_bytes());
        }
        (&file).write_all(&data).expect("shm write");
        let pool = self.shm.as_ref().unwrap().create_pool(file.as_fd(), len as i32, qh, ());
        let buffer = pool.create_buffer(0, w, h, stride, wl_shm::Format::Xrgb8888, qh, ());
        pool.destroy();
        surface.attach(Some(&buffer), 0, 0);
        surface.damage_buffer(0, 0, w, h);
    }

    fn is_sub(&self, surface: &wl_surface::WlSurface) -> bool {
        self.sub.as_ref().is_some_and(|(s, _)| s == surface)
    }

    /// fill surface with color; the frame callback says which surface
    fn paint(
        &self,
        surface: &wl_surface::WlSurface,
        (w, h): (i32, i32),
        color: u32,
        popup: bool,
        qh: &QueueHandle<App>,
    ) {
        let stride = w * 4;
        let len = (stride * h) as usize;
        let file = shm_file(len);
        {
            let mut data = Vec::with_capacity(len);
            for _ in 0..(w * h) {
                data.extend_from_slice(&color.to_le_bytes());
            }
            (&file).write_all(&data).expect("shm write");
        }
        let shm = self.shm.as_ref().unwrap();
        let pool = shm.create_pool(file.as_fd(), len as i32, qh, ());
        let format = if self.alpha {
            wl_shm::Format::Argb8888
        } else {
            wl_shm::Format::Xrgb8888
        };
        let buffer = pool.create_buffer(0, w, h, stride, format, qh, ());
        pool.destroy();
        surface.attach(Some(&buffer), 0, 0);
        surface.damage_buffer(0, 0, w, h);
        surface.frame(qh, (w, h, popup));
        surface.commit();
    }

    /// open the --popup, grabbing with serial if given
    fn open_popup(&mut self, qh: &QueueHandle<App>, grab: Option<u32>) {
        let Some(spec) = &self.popup_spec else {
            return;
        };
        if self.popup_opened {
            return;
        }
        self.popup_opened = true;
        let wm = self.wm_base.as_ref().unwrap();
        let positioner = wm.create_positioner(qh, ());
        positioner.set_size(spec.size.0, spec.size.1);
        positioner.set_anchor_rect(spec.at.0, spec.at.1, 1, 1);
        positioner.set_anchor(xdg_positioner::Anchor::TopLeft);
        positioner.set_gravity(xdg_positioner::Gravity::BottomRight);
        if spec.slide {
            positioner.set_constraint_adjustment(
                xdg_positioner::ConstraintAdjustment::SlideX
                    | xdg_positioner::ConstraintAdjustment::SlideY,
            );
        }
        let surface = self.compositor.as_ref().unwrap().create_surface(qh, ());
        let xs = wm.get_xdg_surface(&surface, qh, true);
        let popup = xs.get_popup(self.xdg_surface.as_ref(), &positioner, qh, ());
        positioner.destroy();
        if let (Some(serial), Some(seat)) = (grab, &self.seat) {
            popup.grab(seat, serial);
        }
        surface.commit();
        self.popup = Some((surface, xs, popup));
    }

    fn is_popup(&self, surface: &wl_surface::WlSurface) -> bool {
        self.popup.as_ref().is_some_and(|(s, _, _)| s == surface)
    }
}

fn shm_file(len: usize) -> File {
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let path = format!("{dir}/wx11-test-client-{}-{len}", std::process::id());
    let file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(&path)
        .expect("shm file");
    let _ = std::fs::remove_file(&path);
    file
}

impl Dispatch<wl_registry::WlRegistry, ()> for App {
    fn event(
        app: &mut App,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        if let wl_registry::Event::Global {
            name, interface, ..
        } = event
        {
            match interface.as_str() {
                "wl_compositor" => app.compositor = Some(registry.bind(name, 4, qh, ())),
                "wl_shm" => app.shm = Some(registry.bind(name, 1, qh, ())),
                "wl_subcompositor" => app.subcompositor = Some(registry.bind(name, 1, qh, ())),
                "xdg_wm_base" => app.wm_base = Some(registry.bind(name, 1, qh, ())),
                "wl_seat" => app.seat = Some(registry.bind(name, 5, qh, ())),
                "wl_output" => {
                    let _: wl_output::WlOutput = registry.bind(name, 2, qh, ());
                }
                "wl_data_device_manager" => app.data_manager = Some(registry.bind(name, 3, qh, ())),
                "zwp_primary_selection_device_manager_v1" => {
                    app.primary_manager = Some(registry.bind(name, 1, qh, ()))
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<xdg_wm_base::XdgWmBase, ()> for App {
    fn event(
        _: &mut App,
        wm: &xdg_wm_base::XdgWmBase,
        event: xdg_wm_base::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let xdg_wm_base::Event::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<xdg_surface::XdgSurface, bool> for App {
    fn event(
        app: &mut App,
        xs: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        popup: &bool,
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xs.ack_configure(serial);
            if *popup {
                let spec = app.popup_spec.as_ref().unwrap();
                let surface = app.popup.as_ref().unwrap().0.clone();
                app.paint(&surface, spec.size, spec.color, true, qh);
                return;
            }
            if let Some((w, h)) = app.asked.take() {
                if w > 0 && h > 0 {
                    app.size = (w, h);
                }
            }
            app.draw(qh);
        }
    }
}

impl Dispatch<xdg_toplevel::XdgToplevel, ()> for App {
    fn event(
        app: &mut App,
        _: &xdg_toplevel::XdgToplevel,
        event: xdg_toplevel::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        match event {
            xdg_toplevel::Event::Configure { width, height, .. } => {
                if width > 0 && height > 0 {
                    say(format!("configure {width} {height}"));
                }
                app.asked = Some((width, height));
            }
            xdg_toplevel::Event::Close => {
                say("closed".into());
                app.exit = true;
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_callback::WlCallback, (i32, i32, bool)> for App {
    fn event(
        app: &mut App,
        _: &wl_callback::WlCallback,
        event: wl_callback::Event,
        &(w, h, popup): &(i32, i32, bool),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            if popup {
                say("popup drawn".into());
                return;
            }
            say(format!("drawn {w} {h}"));
            // --sub-recolor: the desynchronized subsurface changes alone
            let recolor = app.sub_spec.as_mut().and_then(|s| s.recolor.take().map(|c| (c, s.size)));
            if let (Some((color, size)), Some((child, _))) = (recolor, app.sub.clone()) {
                app.fill(&child, size, color, qh);
                child.commit();
                say("sub recolored".into());
            }
            if app.popup_spec.as_ref().is_some_and(|p| !p.grab) {
                app.open_popup(qh, None);
            }
        }
    }
}

impl Dispatch<xdg_popup::XdgPopup, ()> for App {
    fn event(
        app: &mut App,
        _: &xdg_popup::XdgPopup,
        event: xdg_popup::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        match event {
            xdg_popup::Event::Configure {
                x,
                y,
                width,
                height,
            } => say(format!("popup configure {x} {y} {width} {height}")),
            xdg_popup::Event::PopupDone => {
                say("popup done".into());
                if let Some((surface, xs, popup)) = app.popup.take() {
                    popup.destroy();
                    xs.destroy();
                    surface.destroy();
                }
                app.popup_opened = false;
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for App {
    fn event(
        _: &mut App,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        if let wl_seat::Event::Capabilities {
            capabilities: WEnum::Value(caps),
        } = event
        {
            if caps.contains(wl_seat::Capability::Keyboard) {
                seat.get_keyboard(qh, ());
            }
            if caps.contains(wl_seat::Capability::Pointer) {
                seat.get_pointer(qh, ());
            }
        }
    }
}

fn pressed(state: WEnum<wl_keyboard::KeyState>) -> &'static str {
    match state {
        WEnum::Value(wl_keyboard::KeyState::Pressed) => "pressed",
        _ => "released",
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for App {
    fn event(
        app: &mut App,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        match event {
            wl_keyboard::Event::Keymap { size, .. } if size > 0 => say("keymap".into()),
            wl_keyboard::Event::Enter { surface, .. } if app.is_popup(&surface) => {
                say("keyboard enter popup".into())
            }
            wl_keyboard::Event::Enter { serial, .. } => {
                say("keyboard enter".into());
                // copy once we may: with the serial of the focus
                if app.primary {
                    if let (Some(text), Some(manager), Some(device)) =
                        (app.copy.take(), &app.primary_manager, &app.primary_device)
                    {
                        let source = manager.create_source(qh, text);
                        source.offer(mime());
                        device.set_selection(Some(&source), serial);
                        say("copied".into());
                    }
                } else if let (Some(text), Some(manager), Some(device)) =
                    (app.copy.take(), &app.data_manager, &app.data_device)
                {
                    let source = manager.create_data_source(qh, text);
                    source.offer(mime());
                    device.set_selection(Some(&source), serial);
                    say("copied".into());
                }
            }
            wl_keyboard::Event::Leave { .. } => say("keyboard leave".into()),
            wl_keyboard::Event::Key { key, state, .. } => {
                say(format!("key {key} {}", pressed(state)))
            }
            wl_keyboard::Event::Modifiers { mods_depressed, .. } => {
                say(format!("modifiers {mods_depressed}"))
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for App {
    fn event(
        app: &mut App,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                surface,
                surface_x,
                surface_y,
                ..
            } => {
                let what = if app.is_popup(&surface) {
                    " popup"
                } else if app.is_sub(&surface) {
                    " sub"
                } else {
                    ""
                };
                say(format!("pointer enter{what} {surface_x} {surface_y}"))
            }
            wl_pointer::Event::Leave { .. } => say("pointer leave".into()),
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => say(format!("motion {surface_x} {surface_y}")),
            wl_pointer::Event::Button {
                serial,
                button,
                state,
                ..
            } => {
                let s = match state {
                    WEnum::Value(wl_pointer::ButtonState::Pressed) => "pressed",
                    _ => "released",
                };
                say(format!("button {button:#x} {s}"));
                if s == "pressed" && app.popup_spec.as_ref().is_some_and(|p| p.grab) {
                    app.open_popup(qh, Some(serial));
                }
            }
            wl_pointer::Event::Axis { axis, value, .. } => {
                let a = match axis {
                    WEnum::Value(wl_pointer::Axis::VerticalScroll) => "vertical",
                    _ => "horizontal",
                };
                say(format!("axis {a} {value}"))
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_data_source::WlDataSource, String> for App {
    fn event(
        _: &mut App,
        _: &wl_data_source::WlDataSource,
        event: wl_data_source::Event,
        text: &String,
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let wl_data_source::Event::Send { fd, .. } = event {
            send_copy(fd, text);
        }
    }
}

impl Dispatch<wl_data_device::WlDataDevice, ()> for App {
    fn event(
        app: &mut App,
        _: &wl_data_device::WlDataDevice,
        event: wl_data_device::Event,
        _: &(),
        conn: &Connection,
        _: &QueueHandle<App>,
    ) {
        match &event {
            wl_data_device::Event::DataOffer { .. } => app.offer_mimes.clear(),
            wl_data_device::Event::Enter { serial, id: Some(offer), .. } if app.drop_target => {
                say(format!("drag enter {}", app.offer_mimes.join(",")));
                let want = mime();
                if app.offer_mimes.contains(&want) {
                    offer.accept(*serial, Some(want));
                    offer.set_actions(
                        wl_data_device_manager::DndAction::Copy,
                        wl_data_device_manager::DndAction::Copy,
                    );
                } else {
                    offer.accept(*serial, None);
                }
                app.drag_offer = Some(offer.clone());
            }
            wl_data_device::Event::Leave if app.drop_target => say("drag leave".into()),
            wl_data_device::Event::Drop if app.drop_target => {
                if let Some(offer) = app.drag_offer.take() {
                    use std::io::Read;
                    let (mut reader, writer) = std::io::pipe().expect("pipe");
                    offer.receive(mime(), writer.as_fd());
                    drop(writer);
                    let _ = conn.flush();
                    let mut text = Vec::new();
                    let _ = reader.read_to_end(&mut text);
                    offer.finish();
                    offer.destroy();
                    let _ = conn.flush();
                    say(format!("dropped {}", String::from_utf8_lossy(&text)));
                }
            }
            _ => {}
        }
        let wl_data_device::Event::Selection { id: Some(offer) } = event else {
            return;
        };
        if !app.paste || app.primary {
            return;
        }
        let (reader, writer) = std::io::pipe().expect("pipe");
        offer.receive(mime(), writer.as_fd());
        drop(writer);
        let _ = conn.flush();
        print_pasted(reader);
    }

    event_created_child!(App, wl_data_device::WlDataDevice, [
        wl_data_device::EVT_DATA_OFFER_OPCODE => (wl_data_offer::WlDataOffer, ()),
    ]);
}

/// the first connection of a sandboxed run: only the security context
struct Sandbox {
    manager: Option<wp_security_context_manager_v1::WpSecurityContextManagerV1>,
}

impl Dispatch<wl_registry::WlRegistry, ()> for Sandbox {
    fn event(
        sb: &mut Sandbox,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Sandbox>,
    ) {
        if let wl_registry::Event::Global {
            name, interface, ..
        } = event
        {
            if interface == "wp_security_context_manager_v1" {
                sb.manager = Some(registry.bind(name, 1, qh, ()));
            }
        }
    }
}

delegate_noop!(Sandbox: ignore wp_security_context_manager_v1::WpSecurityContextManagerV1);
delegate_noop!(Sandbox: ignore wp_security_context_v1::WpSecurityContextV1);

/// register a security context listener for app_id (engine "wx11-test")
/// and connect through it
fn sandboxed(first: &Connection, app_id: &str) -> (Connection, std::io::PipeWriter) {
    let mut queue = first.new_event_queue();
    let qh = queue.handle();
    first.display().get_registry(&qh, ());
    let mut sb = Sandbox { manager: None };
    queue.roundtrip(&mut sb).expect("roundtrip");
    let Some(manager) = sb.manager.take() else {
        eprintln!("wx11-test-client: no wp_security_context_manager_v1");
        std::process::exit(1);
    };
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".into());
    let path = format!("{dir}/wx11-sandbox-{}", std::process::id());
    let _ = std::fs::remove_file(&path);
    let listener = std::os::unix::net::UnixListener::bind(&path).expect("sandbox socket");
    let (close_r, close_w) = std::io::pipe().expect("pipe");
    let ctx = manager.create_listener(listener.as_fd(), close_r.as_fd(), &qh, ());
    ctx.set_sandbox_engine("wx11-test".into());
    ctx.set_app_id(app_id.into());
    ctx.commit();
    queue.roundtrip(&mut sb).expect("roundtrip");
    let stream = std::os::unix::net::UnixStream::connect(&path).expect("sandbox connect");
    let _ = std::fs::remove_file(&path);
    let conn = Connection::from_socket(stream).expect("sandboxed connection");
    (conn, close_w)
}

/// read what the pipe brings and say "pasted TEXT" (in a thread: the
/// compositor writes it while we go on)
fn print_pasted(reader: std::io::PipeReader) {
    std::thread::spawn(move || {
        use std::io::Read;
        let late = PASTE_LATE_MS.load(std::sync::atomic::Ordering::Relaxed);
        if late > 0 {
            std::thread::sleep(std::time::Duration::from_millis(late));
        }
        let mut text = Vec::new();
        let mut reader = reader;
        let _ = reader.read_to_end(&mut text);
        if late > 0 {
            say(format!("pasted-late {}", text.len()));
        } else if !mime().starts_with("text/") {
            let sum: u64 = text.iter().map(|&b| b as u64).sum();
            say(format!("pasted-bin {} {}", text.len(), sum));
        } else {
            say(format!("pasted {}", String::from_utf8_lossy(&text)));
        }
    });
}

impl Dispatch<zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1, ()> for App {
    fn event(
        app: &mut App,
        _: &zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1,
        event: zwp_primary_selection_device_v1::Event,
        _: &(),
        conn: &Connection,
        _: &QueueHandle<App>,
    ) {
        let zwp_primary_selection_device_v1::Event::Selection { id: Some(offer) } = event else {
            return;
        };
        if !app.paste || !app.primary {
            return;
        }
        let (reader, writer) = std::io::pipe().expect("pipe");
        offer.receive(mime(), writer.as_fd());
        drop(writer);
        let _ = conn.flush();
        print_pasted(reader);
    }

    event_created_child!(App, zwp_primary_selection_device_v1::ZwpPrimarySelectionDeviceV1, [
        zwp_primary_selection_device_v1::EVT_DATA_OFFER_OPCODE =>
            (zwp_primary_selection_offer_v1::ZwpPrimarySelectionOfferV1, ()),
    ]);
}

impl Dispatch<zwp_primary_selection_source_v1::ZwpPrimarySelectionSourceV1, String> for App {
    fn event(
        _: &mut App,
        _: &zwp_primary_selection_source_v1::ZwpPrimarySelectionSourceV1,
        event: zwp_primary_selection_source_v1::Event,
        text: &String,
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let zwp_primary_selection_source_v1::Event::Send { fd, .. } = event {
            send_copy(fd, text);
        }
    }
}

delegate_noop!(App: ignore zwp_primary_selection_offer_v1::ZwpPrimarySelectionOfferV1);
delegate_noop!(App: ignore zwp_primary_selection_device_manager_v1::ZwpPrimarySelectionDeviceManagerV1);
impl Dispatch<wl_data_offer::WlDataOffer, ()> for App {
    fn event(
        app: &mut App,
        _: &wl_data_offer::WlDataOffer,
        event: wl_data_offer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let wl_data_offer::Event::Offer { mime_type } = event {
            if app.paste {
                say(format!("offer {mime_type}"));
            }
            app.offer_mimes.push(mime_type);
        }
    }
}
delegate_noop!(App: ignore wl_data_device_manager::WlDataDeviceManager);
delegate_noop!(App: ignore wl_compositor::WlCompositor);
delegate_noop!(App: ignore wl_subcompositor::WlSubcompositor);
delegate_noop!(App: ignore wl_subsurface::WlSubsurface);
delegate_noop!(App: ignore wl_region::WlRegion);
delegate_noop!(App: ignore xdg_positioner::XdgPositioner);
delegate_noop!(App: ignore wl_shm::WlShm);
delegate_noop!(App: ignore wl_shm_pool::WlShmPool);
delegate_noop!(App: ignore wl_buffer::WlBuffer);

/// "output W H" for the current mode, "output size MMW MMH"
impl Dispatch<wl_output::WlOutput, ()> for App {
    fn event(
        _: &mut App,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        match event {
            wl_output::Event::Mode {
                flags: WEnum::Value(f),
                width,
                height,
                ..
            } if f.contains(wl_output::Mode::Current) => say(format!("output {width} {height}")),
            wl_output::Event::Geometry {
                physical_width,
                physical_height,
                ..
            } => say(format!("output size {physical_width} {physical_height}")),
            _ => {}
        }
    }
}

/// "surface enter output" when the window is said to be on the output
impl Dispatch<wl_surface::WlSurface, ()> for App {
    fn event(
        _: &mut App,
        _: &wl_surface::WlSurface,
        event: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let wl_surface::Event::Enter { .. } = event {
            say("surface enter output".into());
        }
    }
}

fn main() {
    let mut title = "wx11 test".to_string();
    let mut app_id = String::new();
    let mut color = 0x00ff_0000u32;
    let mut size = (64, 48);
    let mut popup_spec = None;
    let mut copy = None;
    let mut paste = false;
    let mut drop_target = false;
    let mut alpha = false;
    let mut security_context = None;
    let mut primary = false;
    let mut sub_spec = None;
    let mut args = std::env::args().skip(1).peekable();
    while let Some(a) = args.next() {
        // --grab takes no value
        let v = if [
            "--grab",
            "--popup-slide",
            "--paste",
            "--drop",
            "--alpha",
            "--primary",
            "--copy-stall",
            "--sub-desync",
            "--sub-no-input",
        ]
        .contains(&a.as_str())
        {
            String::new()
        } else {
            args.next().unwrap_or_default()
        };
        match a.as_str() {
            "--title" => title = v,
            "--app-id" => app_id = v,
            "--color" => color = u32::from_str_radix(&v, 16).expect("--color RRGGBB"),
            "--size" => {
                let (w, h) = v.split_once('x').expect("--size WxH");
                size = (w.parse().expect("width"), h.parse().expect("height"));
            }
            "--popup" => {
                let (wh, xy) = v.split_once('+').expect("--popup WxH+X+Y");
                let (w, h) = wh.split_once('x').expect("--popup WxH+X+Y");
                let (x, y) = xy.split_once('+').expect("--popup WxH+X+Y");
                popup_spec = Some(PopupSpec {
                    size: (w.parse().expect("width"), h.parse().expect("height")),
                    at: (x.parse().expect("x"), y.parse().expect("y")),
                    color: 0x0000_00ff,
                    grab: false,
                    slide: false,
                });
            }
            "--popup-color" => {
                popup_spec.as_mut().expect("--popup first").color =
                    u32::from_str_radix(&v, 16).expect("--popup-color RRGGBB")
            }
            "--subsurface" => {
                let (wh, xy) = v.split_once('+').expect("--subsurface WxH+X+Y");
                let (w, h) = wh.split_once('x').expect("--subsurface WxH+X+Y");
                let (x, y) = xy.split_once('+').expect("--subsurface WxH+X+Y");
                sub_spec = Some(SubSpec {
                    size: (w.parse().expect("width"), h.parse().expect("height")),
                    at: (x.parse().expect("x"), y.parse().expect("y")),
                    color: 0x0000_ff00,
                    desync: false,
                    recolor: None,
                    input_none: false,
                });
            }
            "--sub-color" => {
                sub_spec.as_mut().expect("--subsurface first").color =
                    u32::from_str_radix(&v, 16).expect("--sub-color RRGGBB")
            }
            "--sub-recolor" => {
                sub_spec.as_mut().expect("--subsurface first").recolor =
                    Some(u32::from_str_radix(&v, 16).expect("--sub-recolor RRGGBB"))
            }
            "--sub-desync" => {
                sub_spec.as_mut().expect("--subsurface first").desync = true;
                continue;
            }
            "--sub-no-input" => {
                sub_spec.as_mut().expect("--subsurface first").input_none = true;
                continue;
            }
            "--copy" => copy = Some(v),
            "--mime" => {
                let _ = MIME.set(v);
            }
            "--copy-bytes" => {
                COPY_BYTES.store(
                    v.parse().expect("--copy-bytes N"),
                    std::sync::atomic::Ordering::Relaxed,
                );
                copy.get_or_insert_with(String::new);
            }
            "--paste-late" => PASTE_LATE_MS.store(
                v.parse().expect("--paste-late MS"),
                std::sync::atomic::Ordering::Relaxed,
            ),
            "--copy-stall" => {
                COPY_STALL.store(true, std::sync::atomic::Ordering::Relaxed);
                continue;
            }
            "--security-context" => security_context = Some(v),
            "--drop" => drop_target = true,
            "--paste" => {
                paste = true;
                continue;
            }
            "--alpha" => {
                alpha = true;
                continue;
            }
            "--primary" => {
                primary = true;
                continue;
            }
            "--grab" => {
                popup_spec.as_mut().expect("--popup first").grab = true;
                continue;
            }
            "--popup-slide" => {
                popup_spec.as_mut().expect("--popup first").slide = true;
                continue;
            }
            _ => {
                eprintln!("usage: wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH] [--popup WxH+X+Y [--popup-color RRGGBB] [--grab] [--popup-slide]] [--copy TEXT | --copy-bytes N [--copy-stall]] [--paste [--paste-late MS]] [--drop] [--mime TYPE] [--primary] [--alpha] [--security-context APP_ID] [--subsurface WxH+X+Y [--sub-color RRGGBB] [--sub-desync] [--sub-recolor RRGGBB] [--sub-no-input]]");
                std::process::exit(2);
            }
        }
    }

    let first = Connection::connect_to_env().expect("cannot connect to the Wayland display");
    // a sandboxed run keeps the first connection and the context's close
    // pipe open: the listener lives as long as they do
    let (conn, _keep) = match &security_context {
        Some(app) => {
            let (conn, keep) = sandboxed(&first, app);
            (conn, Some((first.clone(), keep)))
        }
        None => (first.clone(), None),
    };
    let mut queue = conn.new_event_queue();
    let qh = queue.handle();
    conn.display().get_registry(&qh, ());
    let mut app = App {
        subcompositor: None,
        sub_spec,
        sub: None,
        compositor: None,
        shm: None,
        wm_base: None,
        seat: None,
        surface: None,
        xdg_surface: None,
        size,
        asked: None,
        color,
        exit: false,
        popup_spec,
        popup: None,
        popup_opened: false,
        data_manager: None,
        data_device: None,
        copy,
        paste,
        drop_target,
        offer_mimes: Vec::new(),
        drag_offer: None,
        alpha,
        primary,
        primary_manager: None,
        primary_device: None,
    };
    queue.roundtrip(&mut app).expect("roundtrip");
    let (Some(compositor), Some(wm_base)) = (app.compositor.clone(), app.wm_base.clone()) else {
        eprintln!("wx11-test-client: the compositor lacks wl_compositor or xdg_wm_base");
        std::process::exit(1);
    };
    if app.shm.is_none() {
        eprintln!("wx11-test-client: the compositor lacks wl_shm");
        std::process::exit(1);
    }
    if let (Some(manager), Some(seat)) = (&app.data_manager, &app.seat) {
        app.data_device = Some(manager.get_data_device(seat, &qh, ()));
    }
    if let (Some(manager), Some(seat)) = (&app.primary_manager, &app.seat) {
        app.primary_device = Some(manager.get_device(seat, &qh, ()));
    }
    let surface = compositor.create_surface(&qh, ());
    let xs = wm_base.get_xdg_surface(&surface, &qh, false);
    let top = xs.get_toplevel(&qh, ());
    top.set_title(title);
    if !app_id.is_empty() {
        top.set_app_id(app_id);
    }
    surface.commit();
    app.surface = Some(surface);
    app.xdg_surface = Some(xs);

    while !app.exit {
        if queue.blocking_dispatch(&mut app).is_err() {
            break;
        }
    }
}
