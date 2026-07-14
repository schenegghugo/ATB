# Playing online with friends (Tailscale)

ATB's multiplayer is **self-hosted**: one person runs the game server (`tb_server`)
and everyone else connects their game client to it. To do this safely — without
exposing a home IP to the public internet or forwarding router ports — we connect
everyone through **[Tailscale](https://tailscale.com)**, a zero-config mesh VPN
(WireGuard under the hood, free for personal use).

Tailscale gives every machine a stable private address in the `100.x.y.z` range.
Once host and players are on the same *tailnet*, the client talks to the server as
if they were on the same LAN — encrypted end-to-end, and reachable by **nobody
outside the tailnet**.

You install Tailscale once per machine; it is **not** bundled with the game (it is
a privileged system service that must be installed with admin rights, and each
person authenticates to the tailnet with their own login).

---

## 1. Install Tailscale (everyone: host and players)

| OS | Install |
|----|---------|
| **Windows** | `winget install tailscale.tailscale` — or the installer from <https://tailscale.com/download/windows> |
| **macOS** | App Store, or `brew install --cask tailscale` |
| **Arch Linux** | `sudo pacman -S tailscale` |
| **Debian/Ubuntu** | `curl -fsSL https://tailscale.com/install.sh \| sh` |

Then bring it up and log in:

```bash
# Linux — enable the daemon, then authenticate (opens a browser)
sudo systemctl enable --now tailscaled
sudo tailscale up
```

> **Linux gotcha — reboot after a kernel update.** If `tailscaled` won't start and
> `journalctl -u tailscaled` shows `modprobe tun failed … Module tun not found in
> directory /lib/modules/<running-kernel>` (the daemon then loops into
> `start-limit-hit`), a package update replaced the kernel modules for a version
> you haven't booted yet. **Reboot**, then `sudo systemctl start tailscaled`.

On Windows/macOS, launch the Tailscale app and sign in. **Everyone must sign in to
the same tailnet** — the host sends each player an invite from the Tailscale admin
console (<https://login.tailscale.com/admin/machines>), or you all sign in under one
shared account for a small friend group.

Verify the machines can see each other:

```bash
tailscale status      # lists every device on your tailnet
```

---

## 2. Host: run the server

There are **two** host daemons — pick by how your friends will play:

| Daemon | Use it for | Default port |
|--------|-----------|--------------|
| **`tb_lobby`** | The GUI's **"Play Online"** button — the Online Home with seeks, challenges, live + correspondence games. **This is what you almost certainly want.** | 5556 |
| `tb_server` | A single *direct* 1v1 matchmaking queue (the ConnectScreen path), no lobby. | 5555 |

> ⚠️ The GUI's **"Play Online" flow talks to `tb_lobby`**. If you point it at
> `tb_server` instead, the client fails with **"expected a hello message"** — that's
> the match server rejecting the lobby handshake. Run `tb_lobby`.

On the machine that will host (e.g. a spare desktop / mini PC):

```bash
# a) Build the lobby daemon (NOT in the release zip; GUI-free, builds fast).
#    It builds on demand — it is not part of the default `all` target.
cmake --build build --target tb_lobby -j

# b) Find this machine's tailnet IP — players connect to this
tailscale ip -4       # e.g. 100.101.102.103

# c) Run the lobby, bound to that tailnet IP, from the repo/bundle root
#    (so ./data resolves). Bind port 5555 so it matches the client's default
#    connect port; the args are: port, bind-addr, casual-rules, ranked-rules.
./build/tb_lobby 5555 100.101.102.103 data/rules.json data/rules.ranked.json
```

**Bind to the `100.x.y.z` address, not `0.0.0.0`.** This is the whole point: the
server then accepts connections *only* over the tailnet, never the public internet.
Binding `0.0.0.0` (all interfaces) is only safe behind a firewall and is not needed
here.

The two rules files enable both formats through one lobby: `data/rules.json` for
casual play and `data/rules.ranked.json` for rated games.

> **Tip — friendlier address:** enable **MagicDNS** in the Tailscale admin console
> and set a hostname (`sudo tailscale up --hostname=atb-server`). Players can then
> use `atb-server` in place of the numeric IP.

---

## 3. Players: connect the client

Point the game at the host's tailnet IP with `ATB_CONNECT`, and set your login
(accounts auto-register on first use):

```bash
# Linux / macOS
ATB_CONNECT=100.101.102.103:5555 ATB_USER=alice ATB_PASS=hunter2 ./tactical_battler
```

```powershell
# Windows (PowerShell)
$env:ATB_CONNECT="100.101.102.103:5555"; $env:ATB_USER="alice"; $env:ATB_PASS="hunter2"
.\tactical_battler.exe
```

You can also leave the env-vars off and type the server address into the **Connect**
screen's *Server (host:port)* field directly.

### Connection env-vars

| Variable | Meaning |
|----------|---------|
| `ATB_CONNECT` | Server address as `host:port` (port defaults to 5555). |
| `ATB_USER` | Your account name (auto-registered on first login). |
| `ATB_PASS` | Your password. |
| `ATB_LOBBY` | Optional lobby/room id to join. |

---

## 4. Starting a match

In the **`tb_lobby`** (Play Online) flow, once you're in the Online Home you start a
game by posting a **seek** or sending a **direct challenge** to another logged-in
player, then both sides accept — no automatic FIFO pairing. So you need a **second**
player logged in to actually play. For a quick solo test, run a second client on the
host machine (pointed at its own tailnet IP, with a different `ATB_USER`), post a
seek on one, and accept it on the other.

*(The `tb_server` direct-matchmaking daemon is different: it pairs the first two
connected clients **FIFO** with no lobby. Only relevant if you deliberately run
`tb_server` instead.)*

---

## Why not just forward a router port?

Forwarding a port exposes your **home IP** to the whole internet (visible to mass
scanners within hours) and puts the server — which parses untrusted network input —
on the same network as your personal devices. The game's transport is also not yet
TLS-encrypted, so logins would cross the open internet in the clear. Tailscale
avoids all of this: no open ports, no public IP exposure, and WireGuard encrypts the
whole session for free. Use a public VPS with a TLS reverse proxy only when you
outgrow "friends I can invite to my tailnet."
