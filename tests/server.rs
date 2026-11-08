use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::time::Duration;

struct Server(Child);
impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn request(port: u16, method: &str, path: &str) -> Vec<u8> {
    let mut socket = TcpStream::connect(("127.0.0.1", port)).unwrap();
    socket
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    write!(
        socket,
        "{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    .unwrap();
    let mut response = Vec::new();
    socket.read_to_end(&mut response).unwrap();
    response
}

#[test]
fn local_server_serves_only_the_bundled_game() {
    let child = Command::new(env!("CARGO_BIN_EXE_strafe-lab"))
        .args(["--port", "0", "--no-open"])
        .current_dir(std::env::temp_dir())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut server = Server(child);
    let mut line = String::new();
    let mut stdout = BufReader::new(server.0.stdout.take().unwrap());
    stdout.read_line(&mut line).unwrap();
    let port: u16 = line
        .trim()
        .rsplit_once(':')
        .unwrap()
        .1
        .trim_end_matches('/')
        .parse()
        .unwrap();

    let html = request(port, "GET", "/");
    assert!(html.starts_with(b"HTTP/1.1 200 OK"));
    assert!(html
        .windows(b"STRAFE".len())
        .any(|window| window == b"STRAFE"));
    let wasm = request(port, "GET", "/engine.wasm");
    assert!(wasm
        .windows(b"application/wasm".len())
        .any(|window| window == b"application/wasm"));
    assert!(wasm.windows(4).any(|window| window == b"\0asm"));
    assert!(request(port, "GET", "/../../Cargo.toml").starts_with(b"HTTP/1.1 404"));
    assert!(request(port, "POST", "/").starts_with(b"HTTP/1.1 405"));
    let head = request(port, "HEAD", "/");
    assert!(head.ends_with(b"\r\n\r\n"));
}
