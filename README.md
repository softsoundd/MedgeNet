# MedgeNet — Mirror's Edge Online Restoration

<img alt="MedgeNetLogoHigh" src="client/MedgeNetLogoHigh.png" />

MedgeNet is a project that restores online features for Mirror's
Edge (2009) on PC. It includes a Go server and a Windows launcher that redirects
the game to a custom server instead of the retired EA services.

This repository contains the release source for:

- `client/` - the Windows launcher and injected client DLL.
- `server/` - the Go server for accounts, leaderboards, and ghosts.

Disclaimer: MedgeNet is not affiliated with or endorsed by Electronic Arts.

## Features

Full 1:1 replication of original online behaviour is implemented:
- Account and persona creation (for each server).
- Time Trial and Speed Run leaderboard support.
- Ghost upload, listing, download, and playback support.

Support for custom maps or additional behaviour is currently out of scope.

## Running MedgeNet

1. Start `MedgeNetLauncher.exe` as Administrator.
2. Select a local or custom server in the launcher. The [release builds](https://github.com/softsoundd/MedgeNet/releases)
in this repo are already configured to connect to the "main" MedgeNet server
(associated with the [Medge Discord community](https://discord.gg/3tbaHJg)) which includes
[speedrun.com](https://www.speedrun.com/me) level runs in the in-game leaderboards.
    - Alternatively, start the MedgeNet server if running your own instance.
4. Launch Mirror's Edge.
5. Leave the launcher open while playing so it can patch the game process.

The client log is written beside `MedgeNetClient.dll`.

## Build requirements

Client build:

- Windows.
- Visual Studio 2022 with C++ desktop tools.
- Mirror's Edge for PC.

Server build:

- Go 1.25 or newer.
- Administrator/root access if binding HTTP to port `80`.
- Alternatively, Docker or another OCI-compatible runtime for the Linux container image. On
  Windows, use Docker Desktop with Linux containers/WSL 2.

## Project layout

```text
client/
  MedgeNetClient.sln      Visual Studio solution
  MedgeNetClient.ini      Default client/server settings
  dll/                    Injected patch DLL source
  launcher/               Win32 launcher source

server/
  main.go                 Go server entry point
  server.example.ini      Example server configuration
  config/ fesl/ locker/   Server packages
  mlog/ storage/
```

## Build the server

From the `server` directory:

```powershell
go run .
```

To build a Windows server binary:

```powershell
.\build_windows.ps1
```

By default the server listens on:

- FESL: `0.0.0.0:18680`
- HTTP/FileLocker: `0.0.0.0:80`

Runtime data such as the SQLite database, logs and uploaded ghosts are created
beside the running server.

## Run the server with Docker

The server can also run as a Linux/OCI container. From the repository root:

```powershell
docker compose up -d --build
```

It builds `server/Dockerfile` and stores runtime data in the `medgenet-data` Docker volume:

- `/data/me_server.db` - SQLite accounts, personas, leaderboards, metadata.
- `/data/me_server.db-shm` and `/data/me_server.db-wal` - SQLite WAL files.
- `/data/ghosts` - uploaded ghost files.
- `/data/session_*.log` - server logs.

By default the container exposes the same public ports as the non-Docker server:

- FESL: host `18680` -> container `18680`
- HTTP/FileLocker: host `80` -> container `8080`

## Build the client

```text
client\MedgeNetClient.sln
```

Build `Release | Win32`. The build creates `client\dist` with:

```text
MedgeNetLauncher.exe
MedgeNetClient.dll
MedgeNetClient.ini
```

Commandline build:

```bat
msbuild client\MedgeNetClient.sln /p:Configuration=Release /p:Platform=Win32
```

## Client config

The launcher and DLL read `MedgeNetClient.ini`:

```ini
[Server]
Host=127.0.0.1
Port=18680
HTTPPort=80
```

Use `127.0.0.1` for a server running on the same PC. For a public or LAN server,
set `Host` to that server's hostname or IP address. `HTTPPort` must match the
server's HTTP/FileLocker port.
