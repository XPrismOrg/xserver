// SPDX-License-Identifier: MIT
//
// A minimal Wayland client for WX11's tests: one xdg_toplevel filled with
// a colour, from an shm buffer. Prints what happens on stdout, one line
// each: "drawn W H" once the compositor shows a frame, "configure W H"
// for a size asked for, "closed" when asked to close (then exits).
//
//   wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH]

use std::fs::File;
use std::io::Write;
use std::os::fd::AsFd;

use wayland_client::protocol::{
    wl_buffer, wl_callback, wl_compositor, wl_registry, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{delegate_noop, Connection, Dispatch, QueueHandle};
use wayland_protocols::xdg::shell::client::{xdg_surface, xdg_toplevel, xdg_wm_base};

struct App {
    compositor: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    wm_base: Option<xdg_wm_base::XdgWmBase>,
    surface: Option<wl_surface::WlSurface>,
    size: (i32, i32),
    asked: Option<(i32, i32)>,
    color: u32,
    exit: bool,
}

fn say(line: String) {
    let mut out = std::io::stdout();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

impl App {
    fn draw(&mut self, qh: &QueueHandle<App>) {
        let (w, h) = self.size;
        let stride = w * 4;
        let len = (stride * h) as usize;
        let file = shm_file(len);
        {
            let mut data = Vec::with_capacity(len);
            for _ in 0..(w * h) {
                data.extend_from_slice(&self.color.to_le_bytes());
            }
            (&file).write_all(&data).expect("shm write");
        }
        let shm = self.shm.as_ref().unwrap();
        let pool = shm.create_pool(file.as_fd(), len as i32, qh, ());
        let buffer = pool.create_buffer(0, w, h, stride, wl_shm::Format::Xrgb8888, qh, ());
        pool.destroy();
        let surface = self.surface.as_ref().unwrap();
        surface.attach(Some(&buffer), 0, 0);
        surface.damage_buffer(0, 0, w, h);
        surface.frame(qh, (w, h));
        surface.commit();
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

impl Dispatch<xdg_surface::XdgSurface, ()> for App {
    fn event(
        app: &mut App,
        xs: &xdg_surface::XdgSurface,
        event: xdg_surface::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<App>,
    ) {
        if let xdg_surface::Event::Configure { serial } = event {
            xs.ack_configure(serial);
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

impl Dispatch<wl_callback::WlCallback, (i32, i32)> for App {
    fn event(
        _: &mut App,
        _: &wl_callback::WlCallback,
        event: wl_callback::Event,
        size: &(i32, i32),
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            say(format!("drawn {} {}", size.0, size.1));
        }
    }
}

delegate_noop!(App: ignore wl_compositor::WlCompositor);
delegate_noop!(App: ignore wl_shm::WlShm);
delegate_noop!(App: ignore wl_shm_pool::WlShmPool);
delegate_noop!(App: ignore wl_buffer::WlBuffer);
delegate_noop!(App: ignore wl_surface::WlSurface);

fn main() {
    let mut title = "wx11 test".to_string();
    let mut app_id = String::new();
    let mut color = 0x00ff_0000u32;
    let mut size = (64, 48);
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let v = args.next().unwrap_or_default();
        match a.as_str() {
            "--title" => title = v,
            "--app-id" => app_id = v,
            "--color" => color = u32::from_str_radix(&v, 16).expect("--color RRGGBB"),
            "--size" => {
                let (w, h) = v.split_once('x').expect("--size WxH");
                size = (w.parse().expect("width"), h.parse().expect("height"));
            }
            _ => {
                eprintln!("usage: wx11-test-client [--title T] [--app-id ID] [--color RRGGBB] [--size WxH]");
                std::process::exit(2);
            }
        }
    }

    let conn = Connection::connect_to_env().expect("cannot connect to the Wayland display");
    let mut queue = conn.new_event_queue();
    let qh = queue.handle();
    conn.display().get_registry(&qh, ());
    let mut app = App {
        compositor: None,
        shm: None,
        wm_base: None,
        surface: None,
        size,
        asked: None,
        color,
        exit: false,
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
    let surface = compositor.create_surface(&qh, ());
    let xs = wm_base.get_xdg_surface(&surface, &qh, ());
    let top = xs.get_toplevel(&qh, ());
    top.set_title(title);
    if !app_id.is_empty() {
        top.set_app_id(app_id);
    }
    surface.commit();
    app.surface = Some(surface);

    while !app.exit {
        if queue.blocking_dispatch(&mut app).is_err() {
            break;
        }
    }
}
