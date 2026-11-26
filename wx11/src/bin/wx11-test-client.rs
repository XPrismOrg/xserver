// SPDX-License-Identifier: MIT
//
// A minimal Wayland client for WX11's tests: one xdg_toplevel filled with
// a colour, from an shm buffer. Prints what happens on stdout, one line
// each: "drawn W H" once the compositor shows a frame, "configure W H"
// for a size asked for, "closed" when asked to close (then exits), and the
// input it gets: "keymap", "keyboard enter|leave", "key CODE pressed|released"
// (evdev codes), "modifiers DEPRESSED", "pointer enter X Y", "pointer leave",
// "motion X Y", "button CODE pressed|released", "axis vertical|horizontal V".
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
// text it is offered ("pasted TEXT").
//
//   wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH]
//                    [--popup WxH+X+Y [--popup-color RRGGBB] [--grab]]
//                    [--copy TEXT] [--paste] [--alpha] [--security-context APP_ID]

use std::fs::File;
use std::io::Write;
use std::os::fd::AsFd;

use wayland_client::protocol::{
    wl_buffer, wl_callback, wl_compositor, wl_data_device, wl_data_device_manager, wl_data_offer,
    wl_data_source, wl_keyboard, wl_pointer, wl_registry, wl_seat, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{
    delegate_noop, event_created_child, Connection, Dispatch, QueueHandle, WEnum,
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
}

struct App {
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
    alpha: bool,
}

const TEXT: &str = "text/plain;charset=utf-8";

fn say(line: String) {
    let mut out = std::io::stdout();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

impl App {
    fn draw(&mut self, qh: &QueueHandle<App>) {
        let surface = self.surface.clone().unwrap();
        self.paint(&surface, self.size, self.color, false, qh);
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
                "xdg_wm_base" => app.wm_base = Some(registry.bind(name, 1, qh, ())),
                "wl_seat" => app.seat = Some(registry.bind(name, 5, qh, ())),
                "wl_data_device_manager" => app.data_manager = Some(registry.bind(name, 3, qh, ())),
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
                if let (Some(text), Some(manager), Some(device)) =
                    (app.copy.take(), &app.data_manager, &app.data_device)
                {
                    let source = manager.create_data_source(qh, text);
                    source.offer(TEXT.into());
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
                let what = if app.is_popup(&surface) { " popup" } else { "" };
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
            let _ = File::from(fd).write_all(text.as_bytes());
            say("sent".into());
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
        let wl_data_device::Event::Selection { id: Some(offer) } = event else {
            return;
        };
        if !app.paste {
            return;
        }
        let (reader, writer) = std::io::pipe().expect("pipe");
        offer.receive(TEXT.into(), writer.as_fd());
        drop(writer);
        let _ = conn.flush();
        std::thread::spawn(move || {
            use std::io::Read;
            let mut text = String::new();
            let mut reader = reader;
            let _ = reader.read_to_string(&mut text);
            say(format!("pasted {text}"));
        });
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

delegate_noop!(App: ignore wl_data_offer::WlDataOffer);
delegate_noop!(App: ignore wl_data_device_manager::WlDataDeviceManager);
delegate_noop!(App: ignore wl_compositor::WlCompositor);
delegate_noop!(App: ignore xdg_positioner::XdgPositioner);
delegate_noop!(App: ignore wl_shm::WlShm);
delegate_noop!(App: ignore wl_shm_pool::WlShmPool);
delegate_noop!(App: ignore wl_buffer::WlBuffer);
delegate_noop!(App: ignore wl_surface::WlSurface);

fn main() {
    let mut title = "wx11 test".to_string();
    let mut app_id = String::new();
    let mut color = 0x00ff_0000u32;
    let mut size = (64, 48);
    let mut popup_spec = None;
    let mut copy = None;
    let mut paste = false;
    let mut alpha = false;
    let mut security_context = None;
    let mut args = std::env::args().skip(1).peekable();
    while let Some(a) = args.next() {
        // --grab takes no value
        let v = if a == "--grab" || a == "--paste" || a == "--alpha" {
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
                });
            }
            "--popup-color" => {
                popup_spec.as_mut().expect("--popup first").color =
                    u32::from_str_radix(&v, 16).expect("--popup-color RRGGBB")
            }
            "--copy" => copy = Some(v),
            "--security-context" => security_context = Some(v),
            "--paste" => {
                paste = true;
                continue;
            }
            "--alpha" => {
                alpha = true;
                continue;
            }
            "--grab" => {
                popup_spec.as_mut().expect("--popup first").grab = true;
                continue;
            }
            _ => {
                eprintln!("usage: wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH] [--popup WxH+X+Y [--popup-color RRGGBB] [--grab]] [--copy TEXT] [--paste] [--alpha] [--security-context APP_ID]");
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
        alpha,
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
