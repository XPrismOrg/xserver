// SPDX-License-Identifier: MIT
//
// WX11: a Wayland compositor whose only backend is the XPrism server
// (DECISIONS.md AD-3). Each xdg_toplevel becomes a real X window that any
// X11 window manager manages; its buffers are drawn into that window.
//
// This first version copies shm buffers in with PutImage. Zero-copy
// dma-buf (DRI3 PixmapFromBuffers + Present), input, popups, clipboard
// and drag and drop follow (Phase VI).

mod x;

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

use smithay::delegate_compositor;
use smithay::delegate_seat;
use smithay::delegate_shm;
use smithay::delegate_xdg_shell;
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::{EventLoop, Interest, Mode, PostAction};
use smithay::reexports::wayland_server::backend::{ClientData, ClientId, DisconnectReason};
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;
use smithay::reexports::wayland_server::protocol::wl_seat::WlSeat;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{Client, Display, DisplayHandle, Resource};
use smithay::utils::Serial;
use smithay::wayland::buffer::BufferHandler;
use smithay::wayland::compositor::{
    with_states, BufferAssignment, CompositorClientState, CompositorHandler, CompositorState,
    SurfaceAttributes,
};
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
    XdgToplevelSurfaceData,
};
use smithay::wayland::shm::{with_buffer_contents, ShmHandler, ShmState};
use smithay::wayland::socket::ListeningSocketSource;

use x::XSide;

/// A Wayland toplevel shown as an X window.
struct Toplevel {
    surface: ToplevelSurface,
    window: Option<u32>,
    size: (u32, u32),
}

pub struct State {
    dh: DisplayHandle,
    compositor: CompositorState,
    shm: ShmState,
    xdg: XdgShellState,
    seat_state: SeatState<State>,
    #[allow(dead_code)]
    seat: Seat<State>,
    x: XSide,
    toplevels: HashMap<smithay::reexports::wayland_server::backend::ObjectId, Toplevel>,
    start: Instant,
}

#[derive(Default)]
struct ClientState {
    compositor: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, _client_id: ClientId) {}
    fn disconnected(&self, _client_id: ClientId, _reason: DisconnectReason) {}
}

impl CompositorHandler for State {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor
    }

    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor
    }

    fn commit(&mut self, surface: &WlSurface) {
        let id = surface.id();
        let Some(top) = self.toplevels.get(&id) else {
            return;
        };

        // the first commit of a toplevel asks for its configure
        let configured = with_states(surface, |states| {
            states
                .data_map
                .get::<XdgToplevelSurfaceData>()
                .map(|d| d.lock().unwrap().initial_configure_sent)
                .unwrap_or(true)
        });
        if !configured {
            top.surface.send_configure();
            return;
        }

        let (buffer, callbacks) = with_states(surface, |states| {
            let mut attrs = states.cached_state.get::<SurfaceAttributes>();
            let current = attrs.current();
            (
                current.buffer.take(),
                std::mem::take(&mut current.frame_callbacks),
            )
        });

        match buffer {
            Some(BufferAssignment::NewBuffer(buffer)) => self.show_buffer(&id, &buffer),
            Some(BufferAssignment::Removed) => self.hide(&id),
            None => {}
        }

        let time = self.start.elapsed().as_millis() as u32;
        for cb in callbacks {
            cb.done(time);
        }
    }
}

impl State {
    fn show_buffer(
        &mut self,
        id: &smithay::reexports::wayland_server::backend::ObjectId,
        buffer: &WlBuffer,
    ) {
        let mut image = None;
        let _ = with_buffer_contents(buffer, |ptr, len, data| {
            let (w, h, stride, off) = (data.width, data.height, data.stride, data.offset);
            if w <= 0 || h <= 0 || stride < w * 4 || off < 0 {
                return;
            }
            let need = off as usize + stride as usize * h as usize;
            if need > len {
                return;
            }
            // xrgb8888 / argb8888 are BGRX in memory, as a depth 24
            // ZPixmap with 32 bits per pixel
            let mut pixels = Vec::with_capacity(w as usize * h as usize * 4);
            for row in 0..h as usize {
                let start = off as usize + row * stride as usize;
                let src = unsafe { std::slice::from_raw_parts(ptr.add(start), w as usize * 4) };
                pixels.extend_from_slice(src);
            }
            image = Some((w as u32, h as u32, pixels));
        });
        buffer.release();
        let Some((w, h, pixels)) = image else {
            return;
        };

        let top = self.toplevels.get_mut(id).unwrap();
        let (title, app_id) = with_states(top.surface.wl_surface(), |states| {
            let d = states
                .data_map
                .get::<XdgToplevelSurfaceData>()
                .unwrap()
                .lock()
                .unwrap();
            (
                d.title.clone().unwrap_or_default(),
                d.app_id.clone().unwrap_or_default(),
            )
        });
        let window = match top.window {
            Some(win) => {
                if top.size != (w, h) {
                    self.x.resize(win, w, h);
                }
                win
            }
            None => {
                let win = self.x.create_window(w, h, &title, &app_id);
                top.window = Some(win);
                win
            }
        };
        top.size = (w, h);
        self.x.put_image(window, w, h, &pixels);
    }

    fn hide(&mut self, id: &smithay::reexports::wayland_server::backend::ObjectId) {
        if let Some(top) = self.toplevels.get_mut(id) {
            if let Some(win) = top.window.take() {
                self.x.destroy_window(win);
            }
        }
    }

    fn toplevel_for_window(&self, window: u32) -> Option<&Toplevel> {
        self.toplevels.values().find(|t| t.window == Some(window))
    }

    /// what the X server told us: the window manager closing or
    /// resizing a window
    fn handle_x_events(&mut self) {
        for ev in self.x.events() {
            match ev {
                x::Event::Close(win) => {
                    if let Some(t) = self.toplevel_for_window(win) {
                        t.surface.send_close();
                    }
                }
                x::Event::Resized(win, w, h) => {
                    if let Some(t) = self.toplevel_for_window(win) {
                        if t.size != (w, h) {
                            t.surface.with_pending_state(|s| {
                                s.size = Some((w as i32, h as i32).into());
                            });
                            t.surface.send_pending_configure();
                        }
                    }
                }
            }
        }
    }
}

impl SeatHandler for State {
    type KeyboardFocus = WlSurface;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;

    fn seat_state(&mut self) -> &mut SeatState<State> {
        &mut self.seat_state
    }
}

impl BufferHandler for State {
    fn buffer_destroyed(&mut self, _buffer: &WlBuffer) {}
}

impl ShmHandler for State {
    fn shm_state(&self) -> &ShmState {
        &self.shm
    }
}

impl XdgShellHandler for State {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        let id = surface.wl_surface().id();
        self.toplevels.insert(
            id,
            Toplevel {
                surface,
                window: None,
                size: (0, 0),
            },
        );
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let id = surface.wl_surface().id();
        self.hide(&id);
        self.toplevels.remove(&id);
    }

    fn title_changed(&mut self, surface: ToplevelSurface) {
        let id = surface.wl_surface().id();
        let title = with_states(surface.wl_surface(), |states| {
            let d = states
                .data_map
                .get::<XdgToplevelSurfaceData>()
                .unwrap()
                .lock()
                .unwrap();
            d.title.clone().unwrap_or_default()
        });
        if let Some(win) = self.toplevels.get(&id).and_then(|t| t.window) {
            self.x.set_title(win, &title);
        }
    }

    fn new_popup(&mut self, _surface: PopupSurface, _positioner: PositionerState) {}

    fn grab(&mut self, _surface: PopupSurface, _seat: WlSeat, _serial: Serial) {}

    fn reposition_request(
        &mut self,
        _surface: PopupSurface,
        _positioner: PositionerState,
        _token: u32,
    ) {
    }
}

delegate_compositor!(State);
delegate_shm!(State);
delegate_xdg_shell!(State);
delegate_seat!(State);

fn usage() -> ! {
    eprintln!(
        "usage: wx11 [--display DISPLAY] [--socket NAME]\n\
         \n\
         Shows the windows of Wayland applications as X11 windows on the X\n\
         server DISPLAY (default: $DISPLAY). Listens on the Wayland socket NAME\n\
         in $XDG_RUNTIME_DIR (default: the first free wayland-N) and prints\n\
         WAYLAND_DISPLAY=NAME once it accepts clients."
    );
    std::process::exit(2);
}

fn main() {
    let mut display_name: Option<String> = None;
    let mut socket_name: Option<String> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--display" | "-d" => display_name = Some(args.next().unwrap_or_else(|| usage())),
            "--socket" | "-s" => socket_name = Some(args.next().unwrap_or_else(|| usage())),
            "--help" | "-h" => usage(),
            _ => usage(),
        }
    }

    let x = match XSide::connect(display_name.as_deref()) {
        Ok(x) => x,
        Err(e) => {
            eprintln!(
                "wx11: cannot connect to X server {}: {e}",
                display_name
                    .or_else(|| std::env::var("DISPLAY").ok())
                    .unwrap_or_else(|| "(DISPLAY unset)".into())
            );
            std::process::exit(1);
        }
    };

    let mut event_loop: EventLoop<State> = EventLoop::try_new().expect("event loop");
    let display: Display<State> = Display::new().expect("wayland display");
    let dh = display.handle();

    let socket = match socket_name {
        Some(name) => ListeningSocketSource::with_name(&name),
        None => ListeningSocketSource::new_auto(),
    };
    let socket = match socket {
        Ok(s) => s,
        Err(e) => {
            eprintln!("wx11: cannot create the Wayland socket: {e} (is XDG_RUNTIME_DIR set?)");
            std::process::exit(1);
        }
    };
    let name = socket.socket_name().to_string_lossy().into_owned();
    let handle = event_loop.handle();
    handle
        .insert_source(socket, |stream, _, state| {
            if let Err(e) = state
                .dh
                .insert_client(stream, Arc::new(ClientState::default()))
            {
                eprintln!("wx11: cannot add a client: {e}");
            }
        })
        .expect("socket source");
    handle
        .insert_source(
            Generic::new(display, Interest::READ, Mode::Level),
            |_, display, state| {
                // SAFETY: the display is not dropped while the loop runs
                unsafe {
                    display.get_mut().dispatch_clients(state).expect("dispatch");
                }
                Ok(PostAction::Continue)
            },
        )
        .expect("display source");
    handle
        .insert_source(
            Generic::new(x.fd(), Interest::READ, Mode::Level),
            |_, _, state| {
                state.handle_x_events();
                Ok(PostAction::Continue)
            },
        )
        .expect("X source");

    let mut seat_state = SeatState::new();
    let seat = seat_state.new_wl_seat(&dh, "seat0");
    let mut state = State {
        seat_state,
        seat,
        compositor: CompositorState::new::<State>(&dh),
        shm: ShmState::new::<State>(&dh, vec![]),
        xdg: XdgShellState::new::<State>(&dh),
        dh,
        x,
        toplevels: HashMap::new(),
        start: Instant::now(),
    };

    println!("WAYLAND_DISPLAY={name}");
    use std::io::Write;
    let _ = std::io::stdout().flush();

    loop {
        if let Err(e) = event_loop.dispatch(Some(Duration::from_millis(100)), &mut state) {
            eprintln!("wx11: {e}");
            break;
        }
        // events x11rb read while waiting for a reply
        state.handle_x_events();
        let _ = state.dh.flush_clients();
        if !state.x.flush() {
            eprintln!("wx11: lost the X server");
            break;
        }
    }
}
