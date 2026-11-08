//! Tiny dependency-free local asset server. Gameplay runs in the Rust/WASM
//! engine in the window, not on the server or over a network connection.
use std::env;
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

const HTML: &[u8] = include_bytes!("../web/index.html");
const CSS: &[u8] = include_bytes!("../web/style.css");
const JS: &[u8] = include_bytes!("../web/app.js");
const WASM: &[u8] = include_bytes!("../web/engine.wasm");

struct Options {
    host: IpAddr,
    port: u16,
    open: bool,
}

fn options() -> Result<Option<Options>, String> {
    let mut result = Options {
        host: IpAddr::V4(Ipv4Addr::LOCALHOST),
        port: 3000,
        open: true,
    };
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--host" => {
                let value = args.next().ok_or("После --host нужен IP-адрес")?;
                result.host = value
                    .parse()
                    .map_err(|_| format!("Некорректный IP-адрес: {value}"))?;
            }
            "--port" => {
                let value = args.next().ok_or("После --port нужен номер порта")?;
                result.port = value
                    .parse()
                    .map_err(|_| format!("Некорректный порт: {value}"))?;
            }
            "--no-open" => result.open = false,
            "--help" | "-h" => {
                println!("Strafe Lab — ASCII-тренажёр стрейфов\n\nИспользование: strafe-lab [--host IP] [--port ПОРТ] [--no-open]\n\n  --host IP      IP для сервера (по умолчанию 127.0.0.1)\n  --port ПОРТ    порт сервера (по умолчанию 3000, 0 = свободный порт)\n  --no-open      не открывать браузер автоматически\n  -h, --help     эта справка");
                return Ok(None);
            }
            _ => return Err(format!("Неизвестный аргумент: {arg}. Используйте --help.")),
        }
    }
    Ok(Some(result))
}

fn serve(mut stream: TcpStream) -> std::io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    let mut request = [0_u8; 8192];
    let mut used = 0;
    while used < request.len() {
        let count = stream.read(&mut request[used..])?;
        if count == 0 {
            break;
        }
        used += count;
        if request[..used].windows(4).any(|part| part == b"\r\n\r\n") {
            break;
        }
    }
    let line = request[..used]
        .split(|&c| c == b'\n')
        .next()
        .unwrap_or_default();
    let line = std::str::from_utf8(line).unwrap_or("");
    let mut words = line.split_whitespace();
    let method = words.next().unwrap_or("");
    let path = words.next().unwrap_or("").split('?').next().unwrap_or("");
    let (status, content_type, body): (&str, &str, &[u8]) = if method != "GET" && method != "HEAD" {
        (
            "405 Method Not Allowed",
            "text/plain; charset=utf-8",
            b"Method not allowed",
        )
    } else {
        match path {
            "/" | "/index.html" => ("200 OK", "text/html; charset=utf-8", HTML),
            "/style.css" => ("200 OK", "text/css; charset=utf-8", CSS),
            "/app.js" => ("200 OK", "text/javascript; charset=utf-8", JS),
            "/engine.wasm" => ("200 OK", "application/wasm", WASM),
            _ => ("404 Not Found", "text/plain; charset=utf-8", b"Not found"),
        }
    };
    write!(stream, "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", body.len())?;
    if method != "HEAD" {
        stream.write_all(body)?;
    }
    Ok(())
}

fn launch_app(url: &str) -> bool {
    let app_arg = format!("--app={url}");
    let mut candidates: Vec<String> = Vec::new();
    #[cfg(target_os = "windows")]
    {
        for var in ["ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"] {
            if let Some(root) = env::var_os(var) {
                for relative in [
                    r"Microsoft\Edge\Application\msedge.exe",
                    r"Google\Chrome\Application\chrome.exe",
                ] {
                    let path = std::path::Path::new(&root).join(relative);
                    if path.is_file() {
                        candidates.push(path.to_string_lossy().into_owned());
                    }
                }
            }
        }
    }
    #[cfg(target_os = "macos")]
    {
        for path in [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        ] {
            if std::path::Path::new(path).is_file() {
                candidates.push(path.to_owned());
            }
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    candidates.extend(
        [
            "chromium",
            "chromium-browser",
            "google-chrome",
            "microsoft-edge",
        ]
        .into_iter()
        .map(str::to_owned),
    );

    for browser in candidates {
        if Command::new(browser)
            .args([app_arg.as_str(), "--window-size=980,800", "--new-window"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .is_ok()
        {
            return true;
        }
    }
    // App mode is not guaranteed to be installed. The default browser is a
    // useful fallback and still runs the same local Rust/WASM simulation.
    #[cfg(target_os = "windows")]
    let fallback = Command::new("cmd").args(["/C", "start", "", url]).spawn();
    #[cfg(target_os = "macos")]
    let fallback = Command::new("open").arg(url).spawn();
    #[cfg(all(unix, not(target_os = "macos")))]
    let fallback = Command::new("xdg-open")
        .arg(url)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();
    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    let fallback: std::io::Result<std::process::Child> = Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "No browser launcher",
    ));
    fallback.is_ok()
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let options = match options() {
        Ok(Some(options)) => options,
        Ok(None) => return Ok(()),
        Err(error) => return Err(error.into()),
    };
    let listener = TcpListener::bind(SocketAddr::new(options.host, options.port))?;
    let port = listener.local_addr()?.port();
    let display_host = if options.host.is_unspecified() && options.host.is_ipv6() {
        "[::1]".to_owned()
    } else if options.host.is_unspecified() {
        "127.0.0.1".to_owned()
    } else if options.host.is_ipv6() {
        format!("[{}]", options.host)
    } else {
        options.host.to_string()
    };
    let url = format!("http://{display_host}:{port}/");
    println!("Strafe Lab запущен: {url}");
    println!("Оставьте этот терминал открытым. Для остановки нажмите Ctrl+C.");
    if options.host.is_unspecified() {
        println!("Внимание: сервер доступен другим устройствам, которым доступен этот порт.");
    }
    if options.open && !launch_app(&url) {
        println!("Браузер не открылся автоматически — откройте адрес выше вручную.");
    }
    for incoming in listener.incoming() {
        match incoming {
            Ok(stream) => {
                thread::spawn(move || {
                    let _ = serve(stream);
                });
            }
            Err(error) => eprintln!("Ошибка соединения: {error}"),
        }
    }
    Ok(())
}
