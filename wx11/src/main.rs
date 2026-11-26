// SPDX-License-Identifier: MIT
//
// WX11: a Wayland compositor whose only backend is the XPrism server
// (DECISIONS.md AD-3). Each xdg_toplevel becomes a real X window that any
// X11 window manager manages; its buffers are drawn into that window.
//
// Buffers are copied in with PutImage. Input comes from the X window's
// core events: the X server has already decided which client gets it, so
// WX11 only forwards to the toplevel of the window the event arrived on.
// Its keymap is the X server's keyboard layout (_XKB_RULES_NAMES).
//
// Each Wayland client gets an X connection of its own, which creates and
// draws its windows: XPrism sees every Wayland application as a client of
// its own (permissions, prompts, audit), not all of them as WX11.
// Clients that come through a wp_security_context_v1 listener (a sandbox
// such as Flatpak) connect to --sandbox-display instead, for example an
// XPrism namespace socket made with xns-listen. WX11's own connection
// bridges the clipboard.

mod x;

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

use smithay::backend::input::{Axis, AxisSource, ButtonState, KeyState};
use smithay::delegate_compositor;
use smithay::delegate_data_device;
use smithay::delegate_seat;
use smithay::delegate_shm;
use smithay::delegate_xdg_shell;
use smithay::input::keyboard::{FilterResult, KeyboardHandle, XkbConfig};
use smithay::input::pointer::{AxisFrame, ButtonEvent, MotionEvent, PointerHandle};
use smithay::input::{Seat, SeatHandler, SeatState};
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::{
    EventLoop, Interest, LoopHandle, Mode, PostAction, RegistrationToken,
};
use smithay::reexports::wayland_server::backend::{ClientData, ClientId, DisconnectReason};
use smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer;
use smithay::reexports::wayland_server::protocol::wl_callback::WlCallback;
use smithay::reexports::wayland_server::protocol::wl_seat::WlSeat;
use smithay::reexports::wayland_server::protocol::wl_shm;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{Client, Display, DisplayHandle, Resource};
use smithay::utils::{Serial, SERIAL_COUNTER};
use smithay::wayland::buffer::BufferHandler;
use smithay::wayland::compositor::{
    with_states, BufferAssignment, CompositorClientState, CompositorHandler, CompositorState,
    SurfaceAttributes,
};
use smithay::wayland::selection::data_device::{
    clear_data_device_selection, request_data_device_client_selection, set_data_device_focus,
    set_data_device_selection, ClientDndGrabHandler, DataDeviceHandler, DataDeviceState,
    ServerDndGrabHandler,
};
use smithay::wayland::selection::{SelectionHandler, SelectionSource, SelectionTarget};
use smithay::wayland::shell::xdg::{
    PopupSurface, PositionerState, ToplevelSurface, XdgPopupSurfaceData, XdgShellHandler,
    XdgShellState, XdgToplevelSurfaceData,
};
use smithay::wayland::shm::{with_buffer_contents, ShmHandler, ShmState};
use smithay::wayland::socket::ListeningSocketSource;
use std::collections::VecDeque;
use std::os::fd::OwnedFd;

use smithay::delegate_security_context;
use smithay::wayland::security_context::{
    SecurityContext, SecurityContextHandler, SecurityContextListenerSource, SecurityContextState,
};
use std::rc::Rc;
use x::XSide;

type Id = smithay::reexports::wayland_server::backend::ObjectId;

/// A Wayland toplevel shown as an X window.
struct Toplevel {
    surface: ToplevelSurface,
    /// the connection of the client's that made the window
    x: Option<Rc<XSide>>,
    window: Option<u32>,
    size: (u32, u32),
    /// the last frame, to repaint what X lost (Expose)
    pixels: Vec<u8>,
}

/// A popup (menu, tooltip) shown as an override-redirect X window, placed
/// relative to its parent's window. A grabbing popup holds an X pointer
/// grab; a click outside it dismisses it.
struct Popup {
    surface: PopupSurface,
    x: Option<Rc<XSide>>,
    window: Option<u32>,
    size: (u32, u32),
    pixels: Vec<u8>,
    grabbed: bool,
}

pub struct State {
    dh: DisplayHandle,
    compositor: CompositorState,
    shm: ShmState,
    xdg: XdgShellState,
    seat_state: SeatState<State>,
    seat: Seat<State>,
    data_device: DataDeviceState,
    handle: LoopHandle<'static, State>,
    /// the text type a Wayland client's clipboard offers, while it owns
    /// the X CLIPBOARD
    wayland_clipboard: Option<String>,
    /// Wayland clients' pipes waiting for the X clipboard, in order
    to_wayland: VecDeque<OwnedFd>,
    keyboard: KeyboardHandle<State>,
    pointer: PointerHandle<State>,
    /// WX11's own connection (clipboard, keyboard layout)
    x: Rc<XSide>,
    /// each Wayland client's X connection, and its event source
    clients: HashMap<ClientId, (Rc<XSide>, RegistrationToken)>,
    display_name: Option<String>,
    sandbox_display: Option<String>,
    sandbox_warned: bool,
    #[allow(dead_code)]
    security_context: SecurityContextState,
    toplevels: HashMap<Id, Toplevel>,
    popups: HashMap<Id, Popup>,
    start: Instant,
}

#[derive(Default)]
struct ClientState {
    compositor: CompositorClientState,
    /// set for clients that came through a security context (a sandbox)
    sandbox: Option<SecurityContext>,
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
        if self.popups.contains_key(&id) {
            self.commit_popup(surface);
            return;
        }
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

        let (buffer, callbacks) = take_commit(surface);
        match buffer {
            Some(BufferAssignment::NewBuffer(buffer)) => self.show_buffer(&id, &buffer),
            Some(BufferAssignment::Removed) => self.hide(&id),
            None => {}
        }
        self.frame_done(callbacks);
    }
}

/// the pixels of an shm buffer as BGRX rows (width, height, pixels); the
/// buffer is released
/// (and whether it has alpha: argb8888)
fn read_pixels(buffer: &WlBuffer) -> Option<(u32, u32, Vec<u8>, bool)> {
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
        let alpha = data.format == wl_shm::Format::Argb8888;
        image = Some((w as u32, h as u32, pixels, alpha));
    });
    buffer.release();
    image
}

/// take the committed buffer and frame callbacks of surface
fn take_commit(surface: &WlSurface) -> (Option<BufferAssignment>, Vec<WlCallback>) {
    with_states(surface, |states| {
        let mut attrs = states.cached_state.get::<SurfaceAttributes>();
        let current = attrs.current();
        (
            current.buffer.take(),
            std::mem::take(&mut current.frame_callbacks),
        )
    })
}

impl State {
    fn frame_done(&self, callbacks: Vec<WlCallback>) {
        let time = self.start.elapsed().as_millis() as u32;
        for cb in callbacks {
            cb.done(time);
        }
    }

    fn commit_popup(&mut self, surface: &WlSurface) {
        let id = surface.id();
        let configured = with_states(surface, |states| {
            states
                .data_map
                .get::<XdgPopupSurfaceData>()
                .map(|d| d.lock().unwrap().initial_configure_sent)
                .unwrap_or(true)
        });
        if !configured {
            let _ = self.popups[&id].surface.send_configure();
            return;
        }
        let (buffer, callbacks) = take_commit(surface);
        match buffer {
            Some(BufferAssignment::NewBuffer(buffer)) => {
                if let Some((w, h, pixels, alpha)) = read_pixels(&buffer) {
                    self.show_popup(&id, w, h, &pixels, alpha);
                }
            }
            Some(BufferAssignment::Removed) => self.hide_popup(&id),
            None => {}
        }
        self.frame_done(callbacks);
    }

    /// the X window showing surface (a toplevel or a popup)
    fn window_of_surface(&self, surface: &WlSurface) -> Option<u32> {
        let id = surface.id();
        self.toplevels
            .get(&id)
            .and_then(|t| t.window)
            .or_else(|| self.popups.get(&id).and_then(|p| p.window))
    }

    fn show_popup(&mut self, id: &Id, w: u32, h: u32, pixels: &[u8], alpha: bool) {
        let popup = &self.popups[id];
        let parent = popup
            .surface
            .get_parent_surface()
            .and_then(|s| self.window_of_surface(&s));
        let Some(parent) = parent else {
            return; // the parent is not shown (yet)
        };
        let geometry = popup.surface.with_pending_state(|s| s.geometry);
        let wl_surface = popup.surface.wl_surface().clone();
        let Some(conn) = self.conn_for(&wl_surface) else {
            return;
        };
        let (px, py) = conn.root_position(parent);
        let (x, y) = (px + geometry.loc.x, py + geometry.loc.y);
        let popup = self.popups.get_mut(id).unwrap();
        let window = match popup.window {
            Some(win) => {
                conn.move_resize(win, x, y, w, h);
                win
            }
            None => {
                let win = conn.create_popup(x, y, w, h, alpha);
                popup.window = Some(win);
                popup.x = Some(conn.clone());
                if popup.grabbed {
                    conn.grab_pointer(win);
                }
                win
            }
        };
        popup.size = (w, h);
        popup.pixels = pixels.to_vec();
        conn.put_image(window, w, h, pixels);
    }

    fn hide_popup(&mut self, id: &Id) {
        if let Some(popup) = self.popups.get_mut(id) {
            let Some(conn) = popup.x.clone() else {
                return;
            };
            if let Some(win) = popup.window.take() {
                conn.destroy_window(win);
            }
            if popup.grabbed {
                popup.grabbed = false;
                conn.ungrab_pointer();
            }
        }
    }

    /// the toplevel a popup (chain) belongs to
    fn root_toplevel(&self, surface: &WlSurface) -> Option<WlSurface> {
        let mut s = surface.clone();
        loop {
            if self.toplevels.contains_key(&s.id()) {
                return Some(s);
            }
            s = self.popups.get(&s.id())?.surface.get_parent_surface()?;
        }
    }

    /// a click outside the grabbing popups dismisses them (xdg_popup.grab);
    /// true if the click was used for that
    fn dismiss_popups_on_click(&mut self, window: u32, x: f64, y: f64) -> bool {
        let grabbed: Vec<Id> = self
            .popups
            .iter()
            .filter(|(_, p)| p.grabbed)
            .map(|(id, _)| id.clone())
            .collect();
        if grabbed.is_empty() {
            return false;
        }
        let inside = self.popups.values().any(|p| {
            p.window == Some(window)
                && x >= 0.0
                && y >= 0.0
                && x < p.size.0 as f64
                && y < p.size.1 as f64
        });
        if inside {
            return false;
        }
        let mut back_to = None;
        for id in grabbed {
            let p = self.popups.get_mut(&id).unwrap();
            p.grabbed = false;
            p.surface.send_popup_done();
            if let Some(conn) = &p.x {
                conn.ungrab_pointer();
            }
            back_to = self.root_toplevel(self.popups[&id].surface.wl_surface());
        }
        if back_to.is_some() {
            let keyboard = self.keyboard.clone();
            keyboard.set_focus(self, back_to, SERIAL_COUNTER.next_serial());
        }
        true
    }

    fn show_buffer(&mut self, id: &Id, buffer: &WlBuffer) {
        // a window keeps the visual of its first buffer
        let Some((w, h, pixels, alpha)) = read_pixels(buffer) else {
            return;
        };

        let wl_surface = self.toplevels[id].surface.wl_surface().clone();
        let Some(conn) = self.conn_for(&wl_surface) else {
            return;
        };
        let pid = wl_surface
            .client()
            .and_then(|c| c.get_credentials(&self.dh).ok())
            .map(|c| c.pid as u32);
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
                    conn.resize(win, w, h);
                }
                win
            }
            None => {
                let win = conn.create_window(w, h, &title, &app_id, alpha, pid);
                top.window = Some(win);
                top.x = Some(conn.clone());
                win
            }
        };
        top.size = (w, h);
        conn.put_image(window, w, h, &pixels);
        top.pixels = pixels;
    }

    /// X lost part of window (it was covered, without a compositor or
    /// backing store): paint the last frame again
    fn repaint(&self, window: u32) {
        let shown = self
            .toplevel_for_window(window)
            .map(|t| (t.size, &t.pixels, &t.x))
            .or_else(|| {
                self.popups
                    .values()
                    .find(|p| p.window == Some(window))
                    .map(|p| (p.size, &p.pixels, &p.x))
            });
        if let Some(((w, h), pixels, Some(conn))) = shown {
            if pixels.len() == (w * h * 4) as usize {
                conn.put_image(window, w, h, pixels);
            }
        }
    }

    /// the X connection of surface's client, made on first use: on the
    /// sandbox display for clients that came through a security context
    fn conn_for(&mut self, surface: &WlSurface) -> Option<Rc<XSide>> {
        let client = surface.client()?;
        if let Some((conn, _)) = self.clients.get(&client.id()) {
            return Some(conn.clone());
        }
        let sandbox = client
            .get_data::<ClientState>()
            .and_then(|c| c.sandbox.clone());
        let display = match (&sandbox, &self.sandbox_display) {
            (Some(_), Some(d)) => Some(d.clone()),
            (Some(ctx), None) => {
                if !self.sandbox_warned {
                    self.sandbox_warned = true;
                    eprintln!(
                        "wx11: sandboxed client ({:?}) without --sandbox-display: it gets \
                         an ordinary X connection",
                        ctx.app_id
                    );
                }
                self.display_name.clone()
            }
            (None, _) => self.display_name.clone(),
        };
        let conn = match XSide::connect(display.as_deref(), false, self.display_name.as_deref()) {
            Ok(c) => Rc::new(c),
            Err(e) => {
                eprintln!("wx11: cannot connect a client to X ({display:?}): {e}");
                return None;
            }
        };
        let added = self.handle.insert_source(
            Generic::new(conn.fd(), Interest::READ, Mode::Level),
            |_, _, state| {
                state.handle_x_events();
                Ok(PostAction::Continue)
            },
        );
        let token = match added {
            Ok(t) => t,
            Err(e) => {
                eprintln!("wx11: {e}");
                return None;
            }
        };
        self.clients.insert(client.id(), (conn.clone(), token));
        Some(conn)
    }

    /// drop the X connections of Wayland clients that are gone (X then
    /// destroys their windows)
    fn drop_gone_clients(&mut self) {
        let dh = self.dh.clone();
        let gone: Vec<ClientId> = self
            .clients
            .keys()
            .filter(|id| dh.backend_handle().get_client_data((*id).clone()).is_err())
            .cloned()
            .collect();
        for id in gone {
            if let Some((conn, token)) = self.clients.remove(&id) {
                conn.flush();
                // the source holds a copy of the socket: remove it, or X
                // never sees the client go
                self.handle.remove(token);
            }
        }
    }

    /// send every connection's requests; false once WX11's own is gone
    fn flush_x(&self) -> bool {
        for (conn, _) in self.clients.values() {
            conn.flush();
        }
        self.x.flush()
    }

    fn hide(&mut self, id: &Id) {
        if let Some(top) = self.toplevels.get_mut(id) {
            if let (Some(win), Some(conn)) = (top.window.take(), &top.x) {
                conn.destroy_window(win);
            }
        }
    }

    fn toplevel_for_window(&self, window: u32) -> Option<&Toplevel> {
        self.toplevels.values().find(|t| t.window == Some(window))
    }

    fn surface_for_window(&self, window: u32) -> Option<WlSurface> {
        self.toplevel_for_window(window)
            .map(|t| t.surface.wl_surface().clone())
            .or_else(|| {
                self.popups
                    .values()
                    .find(|p| p.window == Some(window))
                    .map(|p| p.surface.wl_surface().clone())
            })
    }

    /// pointer motion inside window (each toplevel's surface is at the
    /// origin of its own X window)
    /// the size of the toplevel or popup shown in window
    fn window_size(&self, window: u32) -> Option<(u32, u32)> {
        self.toplevel_for_window(window)
            .map(|t| t.size)
            .or_else(|| {
                self.popups
                    .values()
                    .find(|p| p.window == Some(window))
                    .map(|p| p.size)
            })
    }

    fn pointer_motion(&mut self, window: u32, x: f64, y: f64, time: u32) {
        // during a popup's grab, X reports motion anywhere relative to the
        // popup: outside its area the pointer is on no surface of ours
        let inside = self
            .window_size(window)
            .is_some_and(|(w, h)| x >= 0.0 && y >= 0.0 && x < w as f64 && y < h as f64);
        let focus = self
            .surface_for_window(window)
            .filter(|_| inside)
            .map(|s| (s, (0.0, 0.0).into()));
        let pointer = self.pointer.clone();
        pointer.motion(
            self,
            focus,
            &MotionEvent {
                location: (x, y).into(),
                serial: SERIAL_COUNTER.next_serial(),
                time,
            },
        );
        pointer.frame(self);
    }

    fn pointer_button(&mut self, button: u8, pressed: bool, time: u32) {
        let pointer = self.pointer.clone();
        // X buttons 4-7 are wheel steps: up, down, left, right
        if (4..=7).contains(&button) {
            if !pressed {
                return;
            }
            let (axis, sign) = match button {
                4 => (Axis::Vertical, -1.0),
                5 => (Axis::Vertical, 1.0),
                6 => (Axis::Horizontal, -1.0),
                _ => (Axis::Horizontal, 1.0),
            };
            let frame = AxisFrame::new(time)
                .source(AxisSource::Wheel)
                .value(axis, sign * 15.0)
                .v120(axis, (sign * 120.0) as i32);
            pointer.axis(self, frame);
            pointer.frame(self);
            return;
        }
        // linux/input-event-codes.h: BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, then
        // BTN_SIDE, BTN_EXTRA for X buttons 8 and 9
        let code = match button {
            1 => 0x110,
            2 => 0x112,
            3 => 0x111,
            8 => 0x113,
            9 => 0x114,
            _ => return,
        };
        pointer.button(
            self,
            &ButtonEvent {
                serial: SERIAL_COUNTER.next_serial(),
                time,
                button: code,
                state: if pressed {
                    ButtonState::Pressed
                } else {
                    ButtonState::Released
                },
            },
        );
        pointer.frame(self);
    }

    fn keyboard_focus(&mut self, window: u32, focused: bool) {
        let surface = self.surface_for_window(window);
        let keyboard = self.keyboard.clone();
        let current = keyboard.current_focus();
        if focused {
            if surface.is_some() && surface != current {
                keyboard.set_focus(self, surface, SERIAL_COUNTER.next_serial());
            }
        } else if surface.is_some() && surface == current {
            keyboard.set_focus(self, None, SERIAL_COUNTER.next_serial());
        }
    }

    fn key(&mut self, window: u32, keycode: u32, pressed: bool, time: u32) {
        // a key event goes to the window X sent it to; make sure that one
        // has the Wayland focus (FocusIn may come after a fast key)
        self.keyboard_focus(window, true);
        let keyboard = self.keyboard.clone();
        keyboard.input::<(), _>(
            self,
            keycode.into(),
            if pressed {
                KeyState::Pressed
            } else {
                KeyState::Released
            },
            SERIAL_COUNTER.next_serial(),
            time,
            |_, _, _| FilterResult::Forward,
        );
    }

    /// what the X server told us: the window manager closing or
    /// resizing a window
    fn handle_x_events(&mut self) {
        let mut conns = vec![self.x.clone()];
        conns.extend(self.clients.values().map(|(c, _)| c.clone()));
        for conn in conns {
            for ev in conn.events() {
                self.handle_x_event(ev);
            }
        }
    }

    fn handle_x_event(&mut self, ev: x::Event) {
        {
            match ev {
                x::Event::Close(win) => {
                    if let Some(t) = self.toplevel_for_window(win) {
                        t.surface.send_close();
                    }
                }
                x::Event::Focus(win, focused) => self.keyboard_focus(win, focused),
                x::Event::Key(win, code, pressed, time) => self.key(win, code, pressed, time),
                x::Event::Enter(win, px, py, time) | x::Event::Motion(win, px, py, time) => {
                    self.pointer_motion(win, px, py, time)
                }
                x::Event::Leave(win, time) => {
                    let pointer = self.pointer.clone();
                    let left = self.surface_for_window(win);
                    if left.is_none() || pointer.current_focus() != left {
                        return;
                    }
                    pointer.motion(
                        self,
                        None,
                        &MotionEvent {
                            location: (0.0, 0.0).into(),
                            serial: SERIAL_COUNTER.next_serial(),
                            time,
                        },
                    );
                    pointer.frame(self);
                }
                x::Event::Button(win, button, pressed, bx, by, time) => {
                    if pressed && self.dismiss_popups_on_click(win, bx, by) {
                        return;
                    }
                    if self.surface_for_window(win).is_some() {
                        self.pointer_button(button, pressed, time)
                    }
                }
                x::Event::ClipboardOwner(owner) => self.x_clipboard_owner(owner),
                x::Event::ClipboardRequest(req) => self.x_clipboard_request(req),
                x::Event::ClipboardData(data) => {
                    if let Some(fd) = self.to_wayland.pop_front() {
                        if let Some(data) = data {
                            // a thread: the client may read slowly
                            std::thread::spawn(move || {
                                use std::io::Write;
                                let _ = std::fs::File::from(fd).write_all(&data);
                            });
                        }
                    }
                }
                x::Event::Expose(win) => self.repaint(win),
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

    /// the client with the keyboard sees the clipboard
    fn focus_changed(&mut self, seat: &Seat<State>, focused: Option<&WlSurface>) {
        let client = focused.and_then(|s| self.dh.get_client(s.id()).ok());
        set_data_device_focus(&self.dh, seat, client);
    }
}

/// the text types WX11 can pass between the clipboards, best first
const TEXT_TYPES: [&str; 4] = [
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain",
    "TEXT",
];

impl State {
    /// another X client took the clipboard (or nobody has it)
    fn x_clipboard_owner(&mut self, owner: Option<u32>) {
        self.wayland_clipboard = None;
        if owner.is_some() {
            let types = TEXT_TYPES.iter().map(|s| s.to_string()).collect();
            set_data_device_selection(&self.dh, &self.seat, types, ());
        } else {
            clear_data_device_selection(&self.dh, &self.seat);
        }
    }

    /// an X client pastes what a Wayland client copied: read it from the
    /// Wayland client through a pipe, then answer
    fn x_clipboard_request(&mut self, req: x::SelectionRequest) {
        let mime = match (&self.wayland_clipboard, self.x.clipboard_target(&req)) {
            (None, _) | (_, x::ClipboardTarget::Other) => {
                return self.x.answer_clipboard(&req, None);
            }
            (_, x::ClipboardTarget::Targets) => return self.x.answer_clipboard(&req, None),
            (Some(mime), _) => mime.clone(),
        };
        let Ok((reader, writer)) = std::io::pipe() else {
            return self.x.answer_clipboard(&req, None);
        };
        if request_data_device_client_selection(&self.seat, mime, writer.into()).is_err() {
            return self.x.answer_clipboard(&req, None);
        }
        let mut data = Vec::new();
        let mut req = Some(req);
        let source = Generic::new(reader, Interest::READ, Mode::Level);
        let added = self.handle.insert_source(source, move |_, reader, state| {
            use std::io::Read;
            let mut chunk = [0u8; 65536];
            // SAFETY: the reader is not replaced or dropped here
            let n = unsafe { reader.get_mut() }.read(&mut chunk).unwrap_or(0);
            // 16 MiB is plenty for text; more is cut off
            if n > 0 && data.len() < 16 << 20 {
                data.extend_from_slice(&chunk[..n]);
                return Ok(PostAction::Continue);
            }
            if let Some(req) = req.take() {
                state.x.answer_clipboard(&req, Some(&data));
                state.x.flush();
            }
            Ok(PostAction::Remove)
        });
        if let Err(e) = added {
            eprintln!("wx11: clipboard: {e}");
        }
    }
}

impl SelectionHandler for State {
    type SelectionUserData = ();

    /// a Wayland client copied (or cleared its selection)
    fn new_selection(
        &mut self,
        ty: SelectionTarget,
        source: Option<SelectionSource>,
        _seat: Seat<State>,
    ) {
        if ty != SelectionTarget::Clipboard {
            return;
        }
        let mimes = source.map(|s| s.mime_types()).unwrap_or_default();
        self.wayland_clipboard = TEXT_TYPES
            .iter()
            .find(|t| mimes.iter().any(|m| m == *t))
            .map(|t| t.to_string());
        if self.wayland_clipboard.is_some() {
            self.x.own_clipboard();
        } else {
            self.x.disown_clipboard();
        }
    }

    /// a Wayland client pastes what an X client copied
    fn send_selection(
        &mut self,
        ty: SelectionTarget,
        _mime_type: String,
        fd: OwnedFd,
        _seat: Seat<State>,
        _user_data: &(),
    ) {
        if ty == SelectionTarget::Clipboard {
            self.to_wayland.push_back(fd);
            self.x.request_clipboard();
        }
    }
}

impl DataDeviceHandler for State {
    fn data_device_state(&self) -> &DataDeviceState {
        &self.data_device
    }
}

impl ClientDndGrabHandler for State {}
impl ServerDndGrabHandler for State {}

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
                pixels: Vec::new(),
                x: None,
            },
        );
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let id = surface.wl_surface().id();
        if self.keyboard.current_focus().as_ref() == Some(surface.wl_surface()) {
            let keyboard = self.keyboard.clone();
            keyboard.set_focus(self, None, SERIAL_COUNTER.next_serial());
        }
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

    fn new_popup(&mut self, surface: PopupSurface, positioner: PositionerState) {
        // placed where the positioner says, relative to the parent; it is
        // not moved to stay on screen (yet)
        surface.with_pending_state(|s| s.geometry = positioner.get_geometry());
        let id = surface.wl_surface().id();
        self.popups.insert(
            id,
            Popup {
                surface,
                window: None,
                size: (0, 0),
                pixels: Vec::new(),
                x: None,
                grabbed: false,
            },
        );
    }

    fn grab(&mut self, surface: PopupSurface, _seat: WlSeat, _serial: Serial) {
        let id = surface.wl_surface().id();
        let Some(popup) = self.popups.get_mut(&id) else {
            return;
        };
        popup.grabbed = true;
        if let (Some(win), Some(conn)) = (popup.window, &popup.x) {
            conn.grab_pointer(win);
        }
        // a grabbing popup (a menu) gets the keyboard
        let keyboard = self.keyboard.clone();
        keyboard.set_focus(
            self,
            Some(surface.wl_surface().clone()),
            SERIAL_COUNTER.next_serial(),
        );
    }

    fn reposition_request(
        &mut self,
        surface: PopupSurface,
        positioner: PositionerState,
        token: u32,
    ) {
        surface.with_pending_state(|s| {
            s.geometry = positioner.get_geometry();
            s.positioner = positioner;
        });
        surface.send_repositioned(token);
        let _ = surface.send_configure();
    }

    fn popup_destroyed(&mut self, surface: PopupSurface) {
        let id = surface.wl_surface().id();
        let had_keyboard = self.keyboard.current_focus().as_ref() == Some(surface.wl_surface());
        let back_to = self.root_toplevel(surface.wl_surface());
        self.hide_popup(&id);
        self.popups.remove(&id);
        if had_keyboard {
            let keyboard = self.keyboard.clone();
            keyboard.set_focus(self, back_to, SERIAL_COUNTER.next_serial());
        }
    }
}

delegate_compositor!(State);
delegate_shm!(State);
delegate_xdg_shell!(State);
delegate_seat!(State);
delegate_data_device!(State);
delegate_security_context!(State);

impl SecurityContextHandler for State {
    /// a sandbox made a listener for an application: its clients are
    /// marked, and get their X connection on --sandbox-display
    fn context_created(&mut self, source: SecurityContextListenerSource, context: SecurityContext) {
        let added = self.handle.insert_source(source, move |stream, _, state| {
            let data = ClientState {
                sandbox: Some(context.clone()),
                ..Default::default()
            };
            if let Err(e) = state.dh.insert_client(stream, Arc::new(data)) {
                eprintln!("wx11: cannot add a sandboxed client: {e}");
            }
        });
        if let Err(e) = added {
            eprintln!("wx11: security context: {e}");
        }
    }
}

fn usage() -> ! {
    eprintln!(
        "usage: wx11 [--display DISPLAY] [--socket NAME] [--sandbox-display DISPLAY]\n\
         \n\
         Shows the windows of Wayland applications as X11 windows on the X\n\
         server DISPLAY (default: $DISPLAY). Listens on the Wayland socket NAME\n\
         in $XDG_RUNTIME_DIR (default: the first free wayland-N) and prints\n\
         WAYLAND_DISPLAY=NAME once it accepts clients. Each Wayland client gets\n\
         an X connection of its own; clients of a sandbox (wp_security_context_v1)\n\
         connect to the sandbox display instead, e.g. an XPrism namespace socket."
    );
    std::process::exit(2);
}

fn main() {
    let mut display_name: Option<String> = None;
    let mut socket_name: Option<String> = None;
    let mut sandbox_display: Option<String> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--display" | "-d" => display_name = Some(args.next().unwrap_or_else(|| usage())),
            "--socket" | "-s" => socket_name = Some(args.next().unwrap_or_else(|| usage())),
            "--sandbox-display" => sandbox_display = Some(args.next().unwrap_or_else(|| usage())),
            "--help" | "-h" => usage(),
            _ => usage(),
        }
    }

    let x = match XSide::connect(display_name.as_deref(), true, None) {
        Ok(x) => Rc::new(x),
        Err(e) => {
            eprintln!(
                "wx11: cannot connect to X server {}: {e}",
                display_name
                    .clone()
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
    let mut seat = seat_state.new_wl_seat(&dh, "seat0");
    let layout = x.keyboard_layout();
    let options = (!layout.options.is_empty()).then(|| layout.options.clone());
    let xkb = XkbConfig {
        rules: &layout.rules,
        model: &layout.model,
        layout: &layout.layout,
        variant: &layout.variant,
        options,
    };
    // the X server's layout; xkbcommon's defaults if it cannot be compiled
    let keyboard = seat
        .add_keyboard(xkb, 600, 25)
        .or_else(|e| {
            eprintln!("wx11: keyboard layout {layout:?}: {e}; using the default layout");
            seat.add_keyboard(XkbConfig::default(), 600, 25)
        })
        .expect("keyboard");
    let pointer = seat.add_pointer();
    let mut state = State {
        seat_state,
        seat,
        data_device: DataDeviceState::new::<State>(&dh),
        handle: event_loop.handle(),
        wayland_clipboard: None,
        to_wayland: VecDeque::new(),
        keyboard,
        pointer,
        compositor: CompositorState::new::<State>(&dh),
        shm: ShmState::new::<State>(&dh, vec![]),
        xdg: XdgShellState::new::<State>(&dh),
        // only clients outside a sandbox may create security contexts
        security_context: SecurityContextState::new::<State, _>(&dh, |client| {
            client
                .get_data::<ClientState>()
                .is_none_or(|c| c.sandbox.is_none())
        }),
        dh,
        x,
        clients: HashMap::new(),
        display_name,
        sandbox_display,
        sandbox_warned: false,
        toplevels: HashMap::new(),
        popups: HashMap::new(),
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
        state.drop_gone_clients();
        if !state.flush_x() {
            eprintln!("wx11: lost the X server");
            break;
        }
    }
}
