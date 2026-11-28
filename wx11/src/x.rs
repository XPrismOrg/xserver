// SPDX-License-Identifier: MIT
//
// The X side of WX11: one X window per Wayland toplevel.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::os::fd::{AsFd, OwnedFd};

use x11rb::connection::{Connection, RequestConnection};
use x11rb::protocol::xfixes::{ConnectionExt as _, SelectionEventMask};
use x11rb::protocol::xproto::{
    AtomEnum, ChangeWindowAttributesAux, ColormapAlloc, ConfigureWindowAux, ConnectionExt,
    CreateGCAux, CreateWindowAux, EventMask, GrabMode, GrabStatus, ImageFormat, NotifyDetail,
    NotifyMode, PropMode, Property, VisualClass, WindowClass,
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
    /// the X clipboard we asked for, in the target (an X atom name or a
    /// MIME type) we asked for it as (None: refused)
    ClipboardData(Sel, String, Option<Vec<u8>>),
    /// the types the X owner of a selection offers (its TARGETS, as atom
    /// names), after request_targets (None: it did not say)
    ClipboardTargets(Sel, Option<Vec<String>>),
    /// another X client took a selection we owned (SelectionClear)
    ClipboardLost(Sel),
    /// an X pointer button went down (true) or up, at x, y in the window
    Button(u32, u8, bool, f64, f64, u32),
    /// an X client drags something over one of our windows (XdndEnter):
    /// the window, the source window and the types it offers (atom names)
    DndEnter(u32, u32, Vec<String>),
    /// the drag moved to x, y on the root window (XdndPosition)
    DndPosition(u32, u32, i32, i32, u32),
    /// the drag from the source window left our window (XdndLeave)
    DndLeave(u32),
    /// it was dropped on our window, at a time (XdndDrop)
    DndDrop(u32, u32),
}

/// the selections WX11 bridges: CLIPBOARD (copy and paste) and PRIMARY
/// (select, then middle-click)
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Sel {
    Clipboard,
    Primary,
    /// what an X client drags (XdndSelection)
    Dnd,
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
    /// another type, by its name: a MIME type such as image/png, which
    /// is what X clients name non-text targets with
    Named(String),
    /// something WX11 does not offer (a target without a name)
    Other,
}

/// a large answer to an X client, given in chunks (ICCCM 2.7.2, INCR):
/// the next chunk goes when the client deletes the property
struct Outgoing {
    type_: u32,
    data: Vec<u8>,
    off: usize,
    started: std::time::Instant,
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
    xdnd: Xdnd,
    /// an InputOnly window that owns and requests selections
    sel_win: u32,
    incr: u32,
    /// INCR transfers coming in (ICCCM 2.7.2): by property, the selection
    /// and the chunks so far
    incoming: RefCell<HashMap<u32, (Sel, String, Vec<u8>)>>,
    /// INCR transfers going out: by requestor window and property
    outgoing: RefCell<HashMap<(u32, u32), Outgoing>>,
    /// atoms by name and names by atom, as interned so far
    atoms: RefCell<HashMap<String, u32>>,
    names: RefCell<HashMap<u32, String>>,
    /// the target each answer property of sel_win was asked for in
    asked: RefCell<HashMap<u32, String>>,
}

/// the atoms of the XDND protocol (drag and drop between X clients,
/// https://freedesktop.org/wiki/Specifications/XDND/), version 5
struct Xdnd {
    aware: u32,
    enter: u32,
    position: u32,
    status: u32,
    leave: u32,
    drop: u32,
    finished: u32,
    selection: u32,
    type_list: u32,
    action_copy: u32,
}

/// the XDND version WX11 speaks
const XDND_VERSION: u32 = 5;

/// largest property WX11 writes in one request; more goes by INCR.
/// WX11_MAX_PROPERTY (bytes) lowers it, for tests.
fn max_property(conn: &RustConnection) -> usize {
    let limit = conn.maximum_request_bytes().saturating_sub(1024).max(4096);
    std::env::var("WX11_MAX_PROPERTY")
        .ok()
        .and_then(|v| v.parse().ok())
        .map_or(limit, |v: usize| v.clamp(1, limit))
}

/// most a pasted X selection may hold; a larger one is cut off
const MAX_INCOMING: usize = 64 << 20;

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
        let incr = atom("INCR")?;
        let xdnd = Xdnd {
            aware: atom("XdndAware")?,
            enter: atom("XdndEnter")?,
            position: atom("XdndPosition")?,
            status: atom("XdndStatus")?,
            leave: atom("XdndLeave")?,
            drop: atom("XdndDrop")?,
            finished: atom("XdndFinished")?,
            selection: atom("XdndSelection")?,
            type_list: atom("XdndTypeList")?,
            action_copy: atom("XdndActionCopy")?,
        };
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
            // the chunks of INCR transfers arrive as property changes
            &CreateWindowAux::new().event_mask(EventMask::PROPERTY_CHANGE),
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
            xdnd,
            sel_win,
            incr,
            incoming: RefCell::new(HashMap::new()),
            outgoing: RefCell::new(HashMap::new()),
            atoms: RefCell::new(HashMap::new()),
            names: RefCell::new(HashMap::new()),
            asked: RefCell::new(HashMap::new()),
        })
    }

    /// the window this connection owns and requests selections with
    pub fn sel_win(&self) -> u32 {
        self.sel_win
    }

    /// read and delete property on sel_win: (type, value)
    fn take_property(&self, property: u32) -> Option<(u32, Vec<u8>)> {
        self.conn
            .get_property(true, self.sel_win, property, AtomEnum::ANY, 0, u32::MAX / 4)
            .ok()?
            .reply()
            .ok()
            .map(|r| (r.type_, r.value))
    }

    fn selection_atom(&self, which: Sel) -> u32 {
        match which {
            Sel::Clipboard => self.clipboard,
            Sel::Primary => AtomEnum::PRIMARY.into(),
            Sel::Dnd => self.xdnd.selection,
        }
    }

    fn which(&self, atom: u32) -> Option<Sel> {
        if atom == self.clipboard {
            Some(Sel::Clipboard)
        } else if atom == u32::from(AtomEnum::PRIMARY) {
            Some(Sel::Primary)
        } else if atom == self.xdnd.selection {
            Some(Sel::Dnd)
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

    /// the atom called name (0 if the server refused)
    pub fn atom(&self, name: &str) -> u32 {
        if let Some(&a) = self.atoms.borrow().get(name) {
            return a;
        }
        let a = self
            .conn
            .intern_atom(false, name.as_bytes())
            .ok()
            .and_then(|c| c.reply().ok())
            .map_or(0, |r| r.atom);
        if a != 0 {
            self.atoms.borrow_mut().insert(name.to_string(), a);
            self.names.borrow_mut().insert(a, name.to_string());
        }
        a
    }

    /// the name of atom (None if it has none)
    pub fn atom_name(&self, atom: u32) -> Option<String> {
        if atom == 0 {
            return None;
        }
        if let Some(n) = self.names.borrow().get(&atom) {
            return Some(n.clone());
        }
        let name = self
            .conn
            .get_atom_name(atom)
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| String::from_utf8_lossy(&r.name).into_owned())?;
        self.names.borrow_mut().insert(atom, name.clone());
        self.atoms.borrow_mut().insert(name.clone(), atom);
        Some(name)
    }

    /// the property of sel_win an answer for (which, target) comes in: one
    /// per selection and target, so answers to requests still out do not
    /// overwrite each other
    fn answer_property(&self, which: Sel, target: &str) -> u32 {
        if target == "UTF8_STRING" {
            // the property earlier versions used
            return match which {
                Sel::Clipboard => self.wx11_selection,
                Sel::Primary => self.wx11_primary,
                Sel::Dnd => self.atom("WX11_XdndSelection"),
            };
        }
        let sel = match which {
            Sel::Clipboard => "CLIPBOARD",
            Sel::Primary => "PRIMARY",
            Sel::Dnd => "XdndSelection",
        };
        self.atom(&format!("WX11_{sel}_{target}"))
    }

    /// ask the X owner of a selection for it as target (an atom name:
    /// UTF8_STRING for text, or a MIME type); the answer comes as
    /// Event::ClipboardData
    pub fn request_clipboard(&self, which: Sel, target: &str) {
        let atom = self.atom(target);
        let property = self.answer_property(which, target);
        if atom == 0 || property == 0 {
            return;
        }
        self.asked.borrow_mut().insert(property, target.to_string());
        let _ = self.conn.convert_selection(
            self.sel_win,
            self.selection_atom(which),
            atom,
            property,
            x11rb::CURRENT_TIME,
        );
    }

    /// ask the X owner of a selection what types it offers; the answer
    /// comes as Event::ClipboardTargets
    pub fn request_targets(&self, which: Sel) {
        let property = self.answer_property(which, "TARGETS");
        let _ = self.conn.convert_selection(
            self.sel_win,
            self.selection_atom(which),
            self.targets,
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
            match self.atom_name(req.target) {
                Some(name) => ClipboardTarget::Named(name),
                None => ClipboardTarget::Other,
            }
        }
    }

    /// ICCCM: an old client may give no property; use the target
    fn reply_property(req: &SelectionRequest) -> u32 {
        if req.property == x11rb::NONE {
            req.target
        } else {
            req.property
        }
    }

    fn notify(&self, req: &SelectionRequest, stored: bool) {
        let event = SelectionNotifyEvent {
            response_type: SELECTION_NOTIFY_EVENT,
            sequence: 0,
            time: req.time,
            requestor: req.requestor,
            selection: req.selection,
            target: req.target,
            property: if stored {
                Self::reply_property(req)
            } else {
                x11rb::NONE
            },
        };
        let _ = self
            .conn
            .send_event(false, req.requestor, EventMask::NO_EVENT, event);
    }

    /// answer TARGETS: TARGETS, the text targets if text is offered, and
    /// each MIME type
    pub fn answer_targets(&self, req: &SelectionRequest, mimes: &[String]) {
        let mut atoms = vec![self.targets];
        if mimes.iter().any(|m| is_text(m)) {
            atoms.extend([self.utf8_string, self.text, AtomEnum::STRING.into()]);
        }
        for m in mimes {
            let a = self.atom(m);
            if a != 0 && !atoms.contains(&a) {
                atoms.push(a);
            }
        }
        let stored = self
            .conn
            .change_property32(
                PropMode::REPLACE,
                req.requestor,
                Self::reply_property(req),
                AtomEnum::ATOM,
                &atoms,
            )
            .is_ok();
        self.notify(req, stored);
    }

    /// answer a request with data in its target (UTF-8 text converted to
    /// Latin-1 for STRING), in chunks (INCR) if it is large; None refuses
    pub fn answer_clipboard(&self, req: &SelectionRequest, data: Option<&[u8]>) {
        let Some(data) = data else {
            return self.notify(req, false);
        };
        let (type_, data): (u32, std::borrow::Cow<[u8]>) = match self.clipboard_target(req) {
            ClipboardTarget::Utf8 => (self.utf8_string, data.into()),
            ClipboardTarget::Latin1 => (
                AtomEnum::STRING.into(),
                String::from_utf8_lossy(data)
                    .chars()
                    .map(|c| if (c as u32) < 256 { c as u8 } else { b'?' })
                    .collect::<Vec<u8>>()
                    .into(),
            ),
            ClipboardTarget::Named(_) => (req.target, data.into()),
            _ => return self.notify(req, false),
        };
        let property = Self::reply_property(req);
        if data.len() <= max_property(&self.conn) {
            let stored = self
                .conn
                .change_property8(PropMode::REPLACE, req.requestor, property, type_, &data)
                .is_ok();
            return self.notify(req, stored);
        }
        // INCR: the size first; each delete of the property asks for the
        // next chunk. A requestor that never deletes it is dropped after
        // a while (when the next transfer starts).
        self.outgoing
            .borrow_mut()
            .retain(|_, o| o.started.elapsed() < std::time::Duration::from_secs(60));
        let watch = self.conn.change_window_attributes(
            req.requestor,
            &ChangeWindowAttributesAux::new().event_mask(EventMask::PROPERTY_CHANGE),
        );
        let stored = watch.is_ok()
            && self
                .conn
                .change_property32(
                    PropMode::REPLACE,
                    req.requestor,
                    property,
                    self.incr,
                    &[data.len().min(u32::MAX as usize) as u32],
                )
                .is_ok();
        if stored {
            self.outgoing.borrow_mut().insert(
                (req.requestor, property),
                Outgoing {
                    type_,
                    data: data.into_owned(),
                    off: 0,
                    started: std::time::Instant::now(),
                },
            );
        }
        self.notify(req, stored);
    }

    /// the requestor of an INCR transfer deleted the property: the next
    /// chunk, or the empty one that ends it
    fn incr_next(&self, window: u32, property: u32) {
        let mut outgoing = self.outgoing.borrow_mut();
        let Some(o) = outgoing.get_mut(&(window, property)) else {
            return;
        };
        let n = (o.data.len() - o.off).min(max_property(&self.conn));
        let _ = self.conn.change_property8(
            PropMode::REPLACE,
            window,
            property,
            o.type_,
            &o.data[o.off..o.off + n],
        );
        o.off += n;
        if n == 0 {
            outgoing.remove(&(window, property));
            // stop hearing about the window unless another transfer to it
            // is still going
            if !outgoing.keys().any(|(w, _)| *w == window) {
                let _ = self.conn.change_window_attributes(
                    window,
                    &ChangeWindowAttributesAux::new().event_mask(EventMask::NO_EVENT),
                );
            }
        }
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
    /// the screen's size in pixels
    pub fn screen_size(&self) -> (i32, i32) {
        let screen = &self.conn.setup().roots[0];
        (
            screen.width_in_pixels as i32,
            screen.height_in_pixels as i32,
        )
    }

    /// the screen's size in millimetres, as the X server reports it
    pub fn screen_mm(&self) -> (i32, i32) {
        let screen = &self.conn.setup().roots[0];
        (
            screen.width_in_millimeters as i32,
            screen.height_in_millimeters as i32,
        )
    }

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
        // a drop target for X clients' drags (XDND)
        let _ = self.conn.change_property32(
            PropMode::REPLACE,
            win,
            self.xdnd.aware,
            AtomEnum::ATOM,
            &[XDND_VERSION],
        );
        self.set_title(win, title);
        let _ = self.conn.map_window(win);
        win
    }

    /// an XDND message to the drag's source window
    fn xdnd_send(&self, source: u32, type_: u32, data: [u32; 5]) {
        let event = x11rb::protocol::xproto::ClientMessageEvent::new(32, source, type_, data);
        let _ = self
            .conn
            .send_event(false, source, EventMask::NO_EVENT, event);
    }

    /// answer XdndPosition: whether window takes the drop (as a copy)
    pub fn dnd_status(&self, window: u32, source: u32, accept: bool) {
        // bit 1: send every position, not only outside a rectangle
        let flags = u32::from(accept) | 2;
        let action = if accept { self.xdnd.action_copy } else { 0 };
        self.xdnd_send(source, self.xdnd.status, [window, flags, 0, 0, action]);
    }

    /// the drop on window is over (taken or not)
    pub fn dnd_finished(&self, window: u32, source: u32, accepted: bool) {
        let action = if accepted { self.xdnd.action_copy } else { 0 };
        self.xdnd_send(source, self.xdnd.finished, [window, u32::from(accepted), action, 0, 0]);
    }

    /// the types an XdndEnter offers: up to three in the message, or all
    /// in the source's XdndTypeList
    fn dnd_types(&self, source: u32, data: [u32; 5]) -> Vec<String> {
        let atoms: Vec<u32> = if data[1] & 1 != 0 {
            self.conn
                .get_property(false, source, self.xdnd.type_list, AtomEnum::ATOM, 0, 1024)
                .ok()
                .and_then(|c| c.reply().ok())
                .and_then(|r| r.value32().map(|v| v.collect()))
                .unwrap_or_default()
        } else {
            data[2..].to_vec()
        };
        atoms.into_iter().filter_map(|a| self.atom_name(a)).collect()
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
                    // the target asked for, by the property it was asked
                    // into: some owners answer with another target
                    let target = self
                        .asked
                        .borrow()
                        .get(&n.property)
                        .cloned()
                        .or_else(|| self.atom_name(n.target))
                        .unwrap_or_default();
                    let got = (n.property != x11rb::NONE)
                        .then(|| self.take_property(n.property))
                        .flatten();
                    match got {
                        Some((_, v)) if n.target == self.targets => {
                            let names = v
                                .chunks_exact(4)
                                .map(|b| u32::from_ne_bytes([b[0], b[1], b[2], b[3]]))
                                .filter_map(|a| self.atom_name(a))
                                .collect();
                            out.push(Event::ClipboardTargets(which, Some(names)));
                        }
                        None if n.target == self.targets => {
                            out.push(Event::ClipboardTargets(which, None))
                        }
                        // large: chunks follow, each as a new value of the
                        // property (deleting it, above, asked for the first)
                        Some((ty, _)) if ty == self.incr => {
                            self.incoming
                                .borrow_mut()
                                .insert(n.property, (which, target, Vec::new()));
                        }
                        got => out.push(Event::ClipboardData(which, target, got.map(|(_, v)| v))),
                    }
                }
                XEvent::PropertyNotify(p)
                    if p.window == self.sel_win
                        && p.state == Property::NEW_VALUE
                        && self.incoming.borrow().contains_key(&p.atom) =>
                {
                    let chunk = self.take_property(p.atom).map(|(_, v)| v);
                    let mut incoming = self.incoming.borrow_mut();
                    let (which, _, data) = incoming.get_mut(&p.atom).unwrap();
                    let which = *which;
                    match chunk {
                        // the empty chunk ends the transfer
                        Some(c) if !c.is_empty() => {
                            let room = MAX_INCOMING.saturating_sub(data.len());
                            data.extend_from_slice(&c[..c.len().min(room)]);
                        }
                        Some(_) => {
                            let (_, target, data) = incoming.remove(&p.atom).unwrap();
                            out.push(Event::ClipboardData(which, target, Some(data)));
                        }
                        None => {
                            let (_, target, _) = incoming.remove(&p.atom).unwrap();
                            out.push(Event::ClipboardData(which, target, None));
                        }
                    }
                }
                // an X client took a chunk of an INCR answer
                XEvent::PropertyNotify(p) if p.state == Property::DELETE => {
                    self.incr_next(p.window, p.atom)
                }
                XEvent::SelectionClear(c) if c.owner == self.sel_win => {
                    if let Some(which) = self.which(c.selection) {
                        out.push(Event::ClipboardLost(which));
                    }
                }
                XEvent::ClientMessage(m) if m.format == 32 && m.type_ == self.xdnd.enter => {
                    let d = m.data.as_data32();
                    // versions before 3 differ; we answer in our own
                    if d[1] >> 24 >= 3 {
                        out.push(Event::DndEnter(m.window, d[0], self.dnd_types(d[0], d)));
                    }
                }
                XEvent::ClientMessage(m) if m.format == 32 && m.type_ == self.xdnd.position => {
                    let d = m.data.as_data32();
                    let (x, y) = ((d[2] >> 16) as i16, (d[2] & 0xffff) as i16);
                    out.push(Event::DndPosition(m.window, d[0], x as i32, y as i32, d[3]));
                }
                XEvent::ClientMessage(m) if m.format == 32 && m.type_ == self.xdnd.leave => {
                    out.push(Event::DndLeave(m.data.as_data32()[0]))
                }
                XEvent::ClientMessage(m) if m.format == 32 && m.type_ == self.xdnd.drop => {
                    let d = m.data.as_data32();
                    out.push(Event::DndDrop(d[0], d[2]))
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

/// a MIME type or X target for text
pub fn is_text(name: &str) -> bool {
    matches!(name, "UTF8_STRING" | "TEXT" | "STRING" | "text/plain")
        || name.starts_with("text/plain;")
}
