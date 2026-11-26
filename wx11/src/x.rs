// SPDX-License-Identifier: MIT
//
// The X side of WX11: one X window per Wayland toplevel.

use std::os::fd::{AsFd, OwnedFd};

use x11rb::connection::{Connection, RequestConnection};
use x11rb::protocol::xproto::{
    AtomEnum, ConfigureWindowAux, ConnectionExt, CreateGCAux, CreateWindowAux, EventMask,
    ImageFormat, NotifyDetail, PropMode, WindowClass,
};
use x11rb::protocol::Event as XEvent;
use x11rb::rust_connection::RustConnection;
use x11rb::wrapper::ConnectionExt as _;

pub enum Event {
    /// the window manager asks to close the window (WM_DELETE_WINDOW)
    Close(u32),
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
    /// an X pointer button went down (true) or up
    Button(u32, u8, bool, u32),
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

pub struct XSide {
    conn: RustConnection,
    root: u32,
    depth: u8,
    visual: u32,
    gc: u32,
    wm_protocols: u32,
    wm_delete_window: u32,
    net_wm_name: u32,
    utf8_string: u32,
    xkb_rules_names: u32,
}

impl XSide {
    pub fn connect(display: Option<&str>) -> Result<XSide, Box<dyn std::error::Error>> {
        let (conn, screen_num) = RustConnection::connect(display)?;
        let screen = &conn.setup().roots[screen_num];
        let (root, depth, visual) = (screen.root, screen.root_depth, screen.root_visual);
        let atom = |name: &str| -> Result<u32, Box<dyn std::error::Error>> {
            Ok(conn.intern_atom(false, name.as_bytes())?.reply()?.atom)
        };
        let wm_protocols = atom("WM_PROTOCOLS")?;
        let wm_delete_window = atom("WM_DELETE_WINDOW")?;
        let net_wm_name = atom("_NET_WM_NAME")?;
        let utf8_string = atom("UTF8_STRING")?;
        let xkb_rules_names = atom("_XKB_RULES_NAMES")?;
        let gc = conn.generate_id()?;
        conn.create_gc(gc, root, &CreateGCAux::new())?;
        Ok(XSide {
            conn,
            root,
            depth,
            visual,
            gc,
            wm_protocols,
            wm_delete_window,
            net_wm_name,
            utf8_string,
            xkb_rules_names,
        })
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

    pub fn create_window(&self, w: u32, h: u32, title: &str, app_id: &str) -> u32 {
        let win = self.conn.generate_id().expect("X resource id");
        let aux = CreateWindowAux::new().background_pixel(0).event_mask(
            EventMask::STRUCTURE_NOTIFY
                | EventMask::FOCUS_CHANGE
                | EventMask::KEY_PRESS
                | EventMask::KEY_RELEASE
                | EventMask::BUTTON_PRESS
                | EventMask::BUTTON_RELEASE
                | EventMask::POINTER_MOTION
                | EventMask::ENTER_WINDOW
                | EventMask::LEAVE_WINDOW,
        );
        let _ = self.conn.create_window(
            self.depth,
            win,
            self.root,
            0,
            0,
            w.max(1) as u16,
            h.max(1) as u16,
            0,
            WindowClass::INPUT_OUTPUT,
            self.visual,
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
        let _ = self.conn.destroy_window(win);
    }

    /// draw BGRX pixels into the window, in as many requests as needed
    pub fn put_image(&self, win: u32, w: u32, h: u32, pixels: &[u8]) {
        let stride = w as usize * 4;
        let max = self.conn.maximum_request_bytes().saturating_sub(64);
        let rows = (max / stride).clamp(1, h as usize);
        let mut y = 0;
        while y < h as usize {
            let n = rows.min(h as usize - y);
            let _ = self.conn.put_image(
                ImageFormat::Z_PIXMAP,
                win,
                self.gc,
                w as u16,
                n as u16,
                0,
                y as i16,
                0,
                self.depth,
                &pixels[y * stride..(y + n) * stride],
            );
            y += n;
        }
    }

    pub fn events(&self) -> Vec<Event> {
        let mut out = Vec::new();
        while let Ok(Some(ev)) = self.conn.poll_for_event() {
            match ev {
                XEvent::ClientMessage(m)
                    if m.type_ == self.wm_protocols
                        && m.data.as_data32()[0] == self.wm_delete_window =>
                {
                    out.push(Event::Close(m.window))
                }
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
                XEvent::ButtonPress(b) => out.push(Event::Button(b.event, b.detail, true, b.time)),
                XEvent::ButtonRelease(b) => {
                    out.push(Event::Button(b.event, b.detail, false, b.time))
                }
                XEvent::MotionNotify(m) => out.push(Event::Motion(
                    m.event,
                    m.event_x as f64,
                    m.event_y as f64,
                    m.time,
                )),
                XEvent::EnterNotify(e) => out.push(Event::Enter(
                    e.event,
                    e.event_x as f64,
                    e.event_y as f64,
                    e.time,
                )),
                XEvent::LeaveNotify(e) => out.push(Event::Leave(e.event, e.time)),
                _ => {}
            }
        }
        out
    }
}
