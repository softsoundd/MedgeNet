# MedgeNet — Mirror's Edge Online Restoration

<img alt="MedgeNetLogoHigh" src="client/MedgeNetLogoHigh.png" />

&nbsp;

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

## Requirements

Client build:

- Windows.
- Visual Studio 2022 with C++ desktop tools.
- Mirror's Edge for PC.

Server build:

- Go 1.25 or newer.
- Administrator/root access if binding HTTP to port `80`.

## Project Layout

```text
client/
  MedgeNetClient.sln      Visual Studio solution
  MedgeNetClient.ini      Default client/server settings
  dll/                    Injected patch DLL source
  launcher/               Win32 launcher source

server/
  main.go                 Go server entry point
  server.example.ini      Example server configuration
  build_windows.ps1       Windows build helper
  config/ fesl/ locker/   Server packages
  mlog/ storage/
```

## Build The Server

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

## Build The Client

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

## Use

1. Start the MedgeNet server.
2. Start `MedgeNetLauncher.exe` as Administrator.
3. Select a local or custom server in the launcher.
4. Launch Mirror's Edge.
5. Leave the launcher open while playing so it can patch the game process.

The client log is written beside `MedgeNetClient.dll`.

