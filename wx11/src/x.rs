// SPDX-License-Identifier: MIT
//
// The X side of WX11: one X window per Wayland toplevel.

use std::cell::RefCell;
use std::collections::HashSet;
use std::os::fd::{AsFd, OwnedFd};

use x11rb::connection::{Connection, RequestConnection};
use x11rb::protocol::xfixes::{ConnectionExt as _, SelectionEventMask};
use x11rb::protocol::xproto::{
    AtomEnum, ColormapAlloc, ConfigureWindowAux, ConnectionExt, CreateGCAux, CreateWindowAux,
    EventMask, GrabMode, GrabStatus, ImageFormat, NotifyDetail, NotifyMode, PropMode, VisualClass,
    WindowClass,
};
use x11rb::protocol::xproto::{SelectionNotifyEvent, SELECTION_NOTIFY_EVENT};
use x11rb::protocol::Event as XEvent;
use x11rb::rust_connection::{DefaultStream, RustConnection};
use x11rb::wrapper::ConnectionExt as _;
use x11rb_protocol::parse_display::parse_display;
use x11rb_protocol::xauth::get_auth;

pub enum Event {
    /// the window manager asks to close the window (WM_DELETE_WINDOW)
    Close(u32),
    /// X lost (part of) the window's contents
    Expose(u32),
    /// the window's size changed
    Resized(u32, u32, u32),
    /// the window got (true) or lost the keyboard focus
    Focus(u32, bool),
    /// a key went down (true) or up; X keycode (= xkb keycode), time
    Key(u32, u32, bool, u32),
    /// the pointer entered the window, at x, y
    Enter(u32, f64, f64, u32),
    /// the pointer left the window
    Leave(u32, u32),
    /// the pointer moved to x, y in the window
    Motion(u32, f64, f64, u32),
    /// another X client owns CLIPBOARD now (None: nobody does)
    ClipboardOwner(Sel, Option<u32>),
    /// an X client asks for our CLIPBOARD (we own it for a Wayland client)
    ClipboardRequest(SelectionRequest),
    /// the X clipboard we asked for (None: refused)
    ClipboardData(Sel, Option<Vec<u8>>),
    /// an X pointer button went down (true) or up, at x, y in the window
    Button(u32, u8, bool, f64, f64, u32),
}

/// the selections WX11 bridges: CLIPBOARD (copy and paste) and PRIMARY
/// (select, then middle-click)
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Sel {
    Clipboard,
    Primary,
}

/// what an X client asked of the clipboard WX11 owns
pub struct SelectionRequest {
    pub which: Sel,
    requestor: u32,
    selection: u32,
    target: u32,
    property: u32,
    time: u32,
}

/// the kinds of clipboard data X clients ask for
pub enum ClipboardTarget {
    /// the list of targets
    Targets,
    /// text as UTF-8 (UTF8_STRING, TEXT)
    Utf8,
    /// text as Latin-1 (STRING)
    Latin1,
    /// something WX11 does not offer
    Other,
}

/// the keyboard layout the X server uses (_XKB_RULES_NAMES), for the
/// Wayland keymap: rules, model, layout, variant, options
#[derive(Default, Debug)]
pub struct KeyboardLayout {
    pub rules: String,
    pub model: String,
    pub layout: String,
    pub variant: String,
    pub options: String,
}

/// connect to the X socket at path with the credentials of auth_display:
/// x11rb could not tell which display's XAUTHORITY entry a bare socket
/// path needs (the socket decides the namespace, the cookie admission)
fn connect_path(
    path: &str,
    auth_display: Option<&str>,
) -> Result<RustConnection, Box<dyn std::error::Error>> {
    let stream = std::os::unix::net::UnixStream::connect(path)?;
    let (stream, (family, address)) = DefaultStream::from_unix_stream(stream)?;
    let display = parse_display(auth_display)?.display;
    let (name, data) = get_auth(family, &address, display)?.unwrap_or_default();
    Ok(RustConnection::connect_to_stream_with_auth_info(
        stream, 0, name, data,
    )?)
}

pub struct XSide {
    conn: RustConnection,
    root: u32,
    depth: u8,
    visual: u32,
    /// a 32-bit TrueColor visual with alpha, a colormap and a GC for it,
    /// for Wayland buffers with alpha (argb8888); a GC only draws on
    /// drawables of its own depth
    argb: Option<(u32, u32, u32)>,
    /// the windows created with the ARGB visual
    deep: RefCell<HashSet<u32>>,
    gc: u32,
    wm_protocols: u32,
    wm_delete_window: u32,
    net_wm_name: u32,
    net_wm_pid: u32,
    utf8_string: u32,
    xkb_rules_names: u32,
    clipboard: u32,
    targets: u32,
    text: u32,
    wx11_selection: u32,
    wx11_primary: u32,
    /// an InputOnly window that owns and requests selections
    sel_win: u32,
}

impl XSide {
    /// connect to display (a display name or a socket path); with
    /// watch_clipboard, watch the CLIPBOARD owner (WX11's own connection).
    /// A socket path (an XPrism namespace socket) gets the credentials of
    /// auth_display, the display it belongs to.
    pub fn connect(
        display: Option<&str>,
        watch_clipboard: bool,
        auth_display: Option<&str>,
    ) -> Result<XSide, Box<dyn std::error::Error>> {
        let (conn, screen_num) = match display {
            Some(path) if path.starts_with('/') => (connect_path(path, auth_display)?, 0),
            _ => RustConnection::connect(display)?,
        };
        let screen = &conn.setup().roots[screen_num];
        let (root, depth, visual) = (screen.root, screen.root_depth, screen.root_visual);
        let argb_visual = screen
            .allowed_depths
            .iter()
            .filter(|d| d.depth == 32)
            .flat_map(|d| d.visuals.iter())
            .find(|v| v.class == VisualClass::TRUE_COLOR)
            .map(|v| v.visual_id);
        let atom = |name: &str| -> Result<u32, Box<dyn std::error::Error>> {
            Ok(conn.intern_atom(false, name.as_bytes())?.reply()?.atom)
        };
        let wm_protocols = atom("WM_PROTOCOLS")?;
        let wm_delete_window = atom("WM_DELETE_WINDOW")?;
        let net_wm_name = atom("_NET_WM_NAME")?;
        let net_wm_pid = atom("_NET_WM_PID")?;
        let utf8_string = atom("UTF8_STRING")?;
        let xkb_rules_names = atom("_XKB_RULES_NAMES")?;
        let clipboard = atom("CLIPBOARD")?;
        let targets = atom("TARGETS")?;
        let text = atom("TEXT")?;
        let wx11_selection = atom("WX11_SELECTION")?;
        let wx11_primary = atom("WX11_PRIMARY")?;
        let gc = conn.generate_id()?;
        conn.create_gc(gc, root, &CreateGCAux::new())?;
        let argb = match argb_visual {
            Some(v) => {
                let cmap = conn.generate_id()?;
                conn.create_colormap(ColormapAlloc::NONE, cmap, root, v)?;
                let pixmap = conn.generate_id()?;
                conn.create_pixmap(32, pixmap, root, 1, 1)?;
                let gc32 = conn.generate_id()?;
                conn.create_gc(gc32, pixmap, &CreateGCAux::new())?;
                conn.free_pixmap(pixmap)?;
                Some((v, cmap, gc32))
            }
            None => None,
        };
        let sel_win = conn.generate_id()?;
        conn.create_window(
            0,
            sel_win,
            root,
            -1,
            -1,
            1,
            1,
            0,
            WindowClass::INPUT_ONLY,
            0,
            &CreateWindowAux::new(),
        )?;
        // hear about every change of the CLIPBOARD and PRIMARY owners
        if watch_clipboard {
            conn.xfixes_query_version(5, 0)?.reply()?;
            for selection in [clipboard, AtomEnum::PRIMARY.into()] {
                conn.xfixes_select_selection_input(
                    sel_win,
                    selection,
                    SelectionEventMask::SET_SELECTION_OWNER
                        | SelectionEventMask::SELECTION_WINDOW_DESTROY
                        | SelectionEventMask::SELECTION_CLIENT_CLOSE,
                )?;
            }
        }
        Ok(XSide {
            conn,
            root,
            depth,
            visual,
            argb,
            deep: RefCell::new(HashSet::new()),
            gc,
            wm_protocols,
            wm_delete_window,
            net_wm_name,
            net_wm_pid,
            utf8_string,
            xkb_rules_names,
            clipboard,
            targets,
            text,
            wx11_selection,
            wx11_primary,
            sel_win,
        })
    }

    fn selection_atom(&self, which: Sel) -> u32 {
        match which {
            Sel::Clipboard => self.clipboard,
            Sel::Primary => AtomEnum::PRIMARY.into(),
        }
    }

    fn which(&self, atom: u32) -> Option<Sel> {
        if atom == self.clipboard {
            Some(Sel::Clipboard)
        } else if atom == u32::from(AtomEnum::PRIMARY) {
            Some(Sel::Primary)
        } else {
            None
        }
    }

    /// own a selection on behalf of a Wayland client
    pub fn own_clipboard(&self, which: Sel) {
        let _ = self.conn.set_selection_owner(
            self.sel_win,
            self.selection_atom(which),
            x11rb::CURRENT_TIME,
        );
    }

    /// give a selection up, if we own it
    pub fn disown_clipboard(&self, which: Sel) {
        let atom = self.selection_atom(which);
        let owner = self
            .conn
            .get_selection_owner(atom)
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| r.owner);
        if owner == Some(self.sel_win) {
            let _ = self
                .conn
                .set_selection_owner(x11rb::NONE, atom, x11rb::CURRENT_TIME);
        }
    }

    /// ask the X owner of a selection for UTF-8 text; the answer comes as
    /// Event::ClipboardData (each selection into its own property)
    pub fn request_clipboard(&self, which: Sel) {
        let property = match which {
            Sel::Clipboard => self.wx11_selection,
            Sel::Primary => self.wx11_primary,
        };
        let _ = self.conn.convert_selection(
            self.sel_win,
            self.selection_atom(which),
            self.utf8_string,
            property,
            x11rb::CURRENT_TIME,
        );
    }

    pub fn clipboard_target(&self, req: &SelectionRequest) -> ClipboardTarget {
        if req.target == self.targets {
            ClipboardTarget::Targets
        } else if req.target == self.utf8_string || req.target == self.text {
            ClipboardTarget::Utf8
        } else if req.target == u32::from(AtomEnum::STRING) {
            ClipboardTarget::Latin1
        } else {
            ClipboardTarget::Other
        }
    }

    /// answer a request: text (UTF-8, converted for STRING), the target
    /// list, or None to refuse
    pub fn answer_clipboard(&self, req: &SelectionRequest, text: Option<&[u8]>) {
        // ICCCM: an old client may give no property; use the target
        let property = if req.property == x11rb::NONE {
            req.target
        } else {
            req.property
        };
        let stored = match self.clipboard_target(req) {
            ClipboardTarget::Targets => self
                .conn
                .change_property32(
                    PropMode::REPLACE,
                    req.requestor,
                    property,
                    AtomEnum::ATOM,
                    &[
                        self.targets,
                        self.utf8_string,
                        self.text,
                        AtomEnum::STRING.into(),
                    ],
                )
                .is_ok(),
            ClipboardTarget::Utf8 if text.is_some() => self
                .conn
                .change_property8(
                    PropMode::REPLACE,
                    req.requestor,
                    property,
                    self.utf8_string,
                    text.unwrap(),
                )
                .is_ok(),
            ClipboardTarget::Latin1 if text.is_some() => {
                let latin1: Vec<u8> = String::from_utf8_lossy(text.unwrap())
                    .chars()
                    .map(|c| if (c as u32) < 256 { c as u8 } else { b'?' })
                    .collect();
                self.conn
                    .change_property8(
                        PropMode::REPLACE,
                        req.requestor,
                        property,
                        AtomEnum::STRING,
                        &latin1,
                    )
                    .is_ok()
            }
            _ => false,
        };
        let event = SelectionNotifyEvent {
            response_type: SELECTION_NOTIFY_EVENT,
            sequence: 0,
            time: req.time,
            requestor: req.requestor,
            selection: req.selection,
            target: req.target,
            property: if stored { property } else { x11rb::NONE },
        };
        let _ = self
            .conn
            .send_event(false, req.requestor, EventMask::NO_EVENT, event);
    }

    /// the keyboard layout set on the X server, empty fields if unknown
    pub fn keyboard_layout(&self) -> KeyboardLayout {
        let value = self
            .conn
            .get_property(
                false,
                self.root,
                self.xkb_rules_names,
                AtomEnum::STRING,
                0,
                1024,
            )
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| r.value)
            .unwrap_or_default();
        let mut f = value
            .split(|&b| b == 0)
            .map(|s| String::from_utf8_lossy(s).into_owned());
        KeyboardLayout {
            rules: f.next().unwrap_or_default(),
            model: f.next().unwrap_or_default(),
            layout: f.next().unwrap_or_default(),
            variant: f.next().unwrap_or_default(),
            options: f.next().unwrap_or_default(),
        }
    }

    pub fn fd(&self) -> OwnedFd {
        self.conn
            .stream()
            .as_fd()
            .try_clone_to_owned()
            .expect("X connection fd")
    }

    /// send what is queued; false once the connection is gone
    pub fn flush(&self) -> bool {
        self.conn.flush().is_ok()
    }

    /// the event mask of every window WX11 shows
    fn event_mask() -> EventMask {
        EventMask::STRUCTURE_NOTIFY
            | EventMask::FOCUS_CHANGE
            | EventMask::KEY_PRESS
            | EventMask::KEY_RELEASE
            | EventMask::BUTTON_PRESS
            | EventMask::BUTTON_RELEASE
            | EventMask::POINTER_MOTION
            | EventMask::ENTER_WINDOW
            | EventMask::LEAVE_WINDOW
            | EventMask::EXPOSURE
    }

    /// an override-redirect window at x, y on the root (a popup: the
    /// window manager does not manage it)
    pub fn create_popup(&self, x: i32, y: i32, w: u32, h: u32, alpha: bool) -> u32 {
        let win = self.conn.generate_id().expect("X resource id");
        let (depth, visual, aux) = self.window_kind(win, alpha);
        let _ = self.conn.create_window(
            depth,
            win,
            self.root,
            x as i16,
            y as i16,
            w.max(1) as u16,
            h.max(1) as u16,
            0,
            WindowClass::INPUT_OUTPUT,
            visual,
            &aux.override_redirect(1),
        );
        let _ = self.conn.map_window(win);
        win
    }

    /// where window's origin is on the root window
    pub fn root_position(&self, win: u32) -> (i32, i32) {
        self.conn
            .translate_coordinates(win, self.root, 0, 0)
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| (r.dst_x as i32, r.dst_y as i32))
            .unwrap_or((0, 0))
    }

    pub fn move_resize(&self, win: u32, x: i32, y: i32, w: u32, h: u32) {
        let _ = self
            .conn
            .configure_window(win, &ConfigureWindowAux::new().x(x).y(y).width(w).height(h));
    }

    /// grab the pointer for a popup: our own windows still get their
    /// events, clicks anywhere else come to win (outside its area)
    pub fn grab_pointer(&self, win: u32) -> bool {
        let mask = EventMask::BUTTON_PRESS | EventMask::BUTTON_RELEASE | EventMask::POINTER_MOTION;
        self.conn
            .grab_pointer(
                true,
                win,
                mask,
                GrabMode::ASYNC,
                GrabMode::ASYNC,
                x11rb::NONE,
                x11rb::NONE,
                x11rb::CURRENT_TIME,
            )
            .ok()
            .and_then(|c| c.reply().ok())
            .is_some_and(|r| r.status == GrabStatus::SUCCESS)
    }

    pub fn ungrab_pointer(&self) {
        let _ = self.conn.ungrab_pointer(x11rb::CURRENT_TIME);
    }

    /// depth, visual and attributes of a new window: with alpha the ARGB
    /// visual (which needs its own colormap and border), if the server
    /// has one; win is remembered as 32-bit
    fn window_kind(&self, win: u32, alpha: bool) -> (u8, u32, CreateWindowAux) {
        let aux = CreateWindowAux::new()
            .background_pixel(0)
            .border_pixel(0)
            .event_mask(Self::event_mask());
        match self.argb {
            Some((visual, cmap, _)) if alpha => {
                self.deep.borrow_mut().insert(win);
                (32, visual, aux.colormap(cmap))
            }
            _ => (self.depth, self.visual, aux),
        }
    }

    /// a top-level window; pid is the Wayland client's process
    /// (_NET_WM_PID), if known
    pub fn create_window(
        &self,
        w: u32,
        h: u32,
        title: &str,
        app_id: &str,
        alpha: bool,
        pid: Option<u32>,
    ) -> u32 {
        let win = self.conn.generate_id().expect("X resource id");
        let (depth, visual, aux) = self.window_kind(win, alpha);
        let _ = self.conn.create_window(
            depth,
            win,
            self.root,
            0,
            0,
            w.max(1) as u16,
            h.max(1) as u16,
            0,
            WindowClass::INPUT_OUTPUT,
            visual,
            &aux,
        );
        let _ = self.conn.change_property32(
            PropMode::REPLACE,
            win,
            self.wm_protocols,
            AtomEnum::ATOM,
            &[self.wm_delete_window],
        );
        // WM_HINTS: flags InputHint, input True, so that window managers
        // give the window the keyboard focus
        let _ = self.conn.change_property32(
            PropMode::REPLACE,
            win,
            AtomEnum::WM_HINTS,
            AtomEnum::WM_HINTS,
            &[1, 1, 0, 0, 0, 0, 0, 0, 0],
        );
        if !app_id.is_empty() {
            // WM_CLASS: instance and class, both the app id
            let class = format!("{app_id}\0{app_id}\0");
            let _ = self.conn.change_property8(
                PropMode::REPLACE,
                win,
                AtomEnum::WM_CLASS,
                AtomEnum::STRING,
                class.as_bytes(),
            );
        }
        if let Some(pid) = pid {
            let _ = self.conn.change_property32(
                PropMode::REPLACE,
                win,
                self.net_wm_pid,
                AtomEnum::CARDINAL,
                &[pid],
            );
        }
        self.set_title(win, title);
        let _ = self.conn.map_window(win);
        win
    }

    pub fn set_title(&self, win: u32, title: &str) {
        let _ = self.conn.change_property8(
            PropMode::REPLACE,
            win,
            self.net_wm_name,
            self.utf8_string,
            title.as_bytes(),
        );
        // WM_NAME for older window managers (Latin-1; others are replaced)
        let latin1: Vec<u8> = title
            .chars()
            .map(|c| if (c as u32) < 256 { c as u8 } else { b'?' })
            .collect();
        let _ = self.conn.change_property8(
            PropMode::REPLACE,
            win,
            AtomEnum::WM_NAME,
            AtomEnum::STRING,
            &latin1,
        );
    }

    pub fn resize(&self, win: u32, w: u32, h: u32) {
        let _ = self
            .conn
            .configure_window(win, &ConfigureWindowAux::new().width(w).height(h));
    }

    pub fn destroy_window(&self, win: u32) {
        self.deep.borrow_mut().remove(&win);
        let _ = self.conn.destroy_window(win);
    }

    /// draw BGRX pixels into the window, in as many requests as needed
    pub fn put_image(&self, win: u32, w: u32, h: u32, pixels: &[u8]) {
        // argb8888 and the ARGB visual are both premultiplied BGRA
        let (depth, gc) = match self.argb {
            Some((_, _, gc32)) if self.deep.borrow().contains(&win) => (32, gc32),
            _ => (self.depth, self.gc),
        };
        let stride = w as usize * 4;
        let max = self.conn.maximum_request_bytes().saturating_sub(64);
        let rows = (max / stride).clamp(1, h as usize);
        let mut y = 0;
        while y < h as usize {
            let n = rows.min(h as usize - y);
            let _ = self.conn.put_image(
                ImageFormat::Z_PIXMAP,
                win,
                gc,
                w as u16,
                n as u16,
                0,
                y as i16,
                0,
                depth,
                &pixels[y * stride..(y + n) * stride],
            );
            y += n;
        }
    }

    pub fn events(&self) -> Vec<Event> {
        let mut out = Vec::new();
        while let Ok(Some(ev)) = self.conn.poll_for_event() {
            match ev {
                XEvent::XfixesSelectionNotify(s) if self.which(s.selection).is_some() => {
                    let owner = (s.owner != x11rb::NONE).then_some(s.owner);
                    if owner != Some(self.sel_win) {
                        out.push(Event::ClipboardOwner(
                            self.which(s.selection).unwrap(),
                            owner,
                        ));
                    }
                }
                XEvent::SelectionRequest(r) if self.which(r.selection).is_some() => {
                    out.push(Event::ClipboardRequest(SelectionRequest {
                        which: self.which(r.selection).unwrap(),
                        requestor: r.requestor,
                        selection: r.selection,
                        target: r.target,
                        property: r.property,
                        time: r.time,
                    }))
                }
                XEvent::SelectionNotify(n)
                    if n.requestor == self.sel_win && self.which(n.selection).is_some() =>
                {
                    let which = self.which(n.selection).unwrap();
                    let data = (n.property != x11rb::NONE)
                        .then(|| {
                            self.conn
                                .get_property(
                                    true,
                                    self.sel_win,
                                    n.property,
                                    AtomEnum::ANY,
                                    0,
                                    u32::MAX / 4,
                                )
                                .ok()
                                .and_then(|c| c.reply().ok())
                                .map(|r| r.value)
                        })
                        .flatten();
                    out.push(Event::ClipboardData(which, data));
                }
                XEvent::ClientMessage(m)
                    if m.type_ == self.wm_protocols
                        && m.data.as_data32()[0] == self.wm_delete_window =>
                {
                    out.push(Event::Close(m.window))
                }
                // the last of a series of exposures: repaint once
                XEvent::Expose(e) if e.count == 0 => out.push(Event::Expose(e.window)),
                XEvent::ConfigureNotify(c) => {
                    out.push(Event::Resized(c.window, c.width as u32, c.height as u32))
                }
                // focus moves between windows (not pointer-only details)
                XEvent::FocusIn(f) if f.detail != NotifyDetail::POINTER => {
                    out.push(Event::Focus(f.event, true))
                }
                XEvent::FocusOut(f) if f.detail != NotifyDetail::POINTER => {
                    out.push(Event::Focus(f.event, false))
                }
                XEvent::KeyPress(k) => out.push(Event::Key(k.event, k.detail as u32, true, k.time)),
                XEvent::KeyRelease(k) => {
                    out.push(Event::Key(k.event, k.detail as u32, false, k.time))
                }
                XEvent::ButtonPress(b) => out.push(Event::Button(
                    b.event,
                    b.detail,
                    true,
                    b.event_x as f64,
                    b.event_y as f64,
                    b.time,
                )),
                XEvent::ButtonRelease(b) => out.push(Event::Button(
                    b.event,
                    b.detail,
                    false,
                    b.event_x as f64,
                    b.event_y as f64,
                    b.time,
                )),
                XEvent::MotionNotify(m) => out.push(Event::Motion(
                    m.event,
                    m.event_x as f64,
                    m.event_y as f64,
                    m.time,
                )),
                // crossings caused by grabbing or ungrabbing are not the
                // pointer moving (a popup's grab starts with one)
                XEvent::EnterNotify(e) if e.mode == NotifyMode::NORMAL => out.push(Event::Enter(
                    e.event,
                    e.event_x as f64,
                    e.event_y as f64,
                    e.time,
                )),
                XEvent::LeaveNotify(e) if e.mode == NotifyMode::NORMAL => {
                    out.push(Event::Leave(e.event, e.time))
                }
                _ => {}
            }
        }
        out
    }
}
