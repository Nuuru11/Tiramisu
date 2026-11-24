# Tiramisu — Setup Guide

Get Tiramisu running on your Xbox in Developer Mode, install it, and put games
on it. No coding needed.

> **New here?** This is the only guide you need. Follow the steps in order.

## What you need

- An Xbox One, One S, One X, Series S, or Series X.
- The Xbox and your phone/computer on the **same Wi-Fi**.
- A Microsoft account (for Dev Mode activation).
- A browser (Edge, Chrome, or Safari).
- About $19 (one time) for a Microsoft Partner Center developer account —
  required to turn on Dev Mode.

## Step 1 — Turn on Developer Mode

1. On the Xbox, open the **Microsoft Store**. Search for **Xbox Dev Mode** and
   install it.
2. Open the app. It shows an **activation code** — write it down.
3. On your phone or computer, register a developer account at Partner Center:
   <https://developer.microsoft.com/en-us/microsoft-store/register/>
   (one-time fee, about $19 for an individual account).
4. In Partner Center, go to **Xbox configuration → Devices**
   (<https://partner.microsoft.com/xboxconfig/devices>), enter the activation
   code, and click **Agree and activate**.
5. Back on the Xbox, open the **Xbox Dev Mode** app and choose **Switch to Dev
   Mode** (or **Switch and restart**). The console restarts.
6. After the restart, open **Dev Home**. This is your Dev Mode home screen.

## Step 2 — Turn on the Device Portal and the memory setting

1. In **Dev Home**, go to **Home → Remote Access → Remote Access Settings**.
2. Turn ON **Enable Xbox Device Portal**.
3. Turn ON **Require authentication**, set a username and password, and save.
   You will use these to sign into the portal website.
4. Note the portal address shown (it looks like `https://10.0.0.23:11443`).
   That IP address is your Xbox's address — you will need it.
5. In **Dev Home**, go to **Settings** and turn ON **"Treat UWP apps as games
   by default"**. This gives Tiramisu 5 GB of memory. Without it, the app can
   close right after opening.

## Step 3 — Download the release

On your phone or computer, open:

<https://github.com/xanstomper/Tiramisu/releases>

Under the newest release, download these two files and keep them together in
one folder:

| File | What it is |
|---|---|
| `Tiramisu.msix` | The app itself |
| `dev.cer` | The signing certificate |

### The `dev.cer` file — just leave it alone

**Do nothing with `dev.cer`.** Do not open it. Do not double-click it. Do not
install it on your computer. Do not put it in the Dependencies box. Just leave
it in the same folder as `Tiramisu.msix` and forget about it.

The installer uses it automatically when it needs it. You never have to touch
it.

(For the experimental keyboard/mouse edition, download
`Tiramisu-EdgeSpoof.msix` + `edge-dev.cer` instead. Do not mix the two pairs —
the normal app needs `dev.cer`, the KBM app needs `edge-dev.cer`.)

## Step 4 — Install Tiramisu

### Try the website first

1. In your browser, open the portal address from Step 2 (for example
   `https://10.0.0.23:11443`).
2. The browser warns the connection is not secure. This is normal — choose
   **Advanced → Continue** (or **Proceed anyway**).
3. Sign in with the username and password you set in Step 2.
4. Go to **My games & apps → Add**.
5. In **App package**, choose `Tiramisu.msix`. Leave **Dependencies** empty.
   Click **Add**.
6. Wait for the page to say the install finished.

**There is no Certificate box on the current Xbox Device Portal.** Do not look
for one. If the install works, you are done — skip to Step 5.

### If the website install fails — use the command-line installer

This is the reliable method. It uploads the `.msix` and the `.cer` together,
so the certificate is handled for you.

1. Install a terminal (one time):
   - **Windows:** install **Git for Windows** from <https://git-scm.com> (gives
     you "Git Bash"), or run `wsl --install` and use Ubuntu.
   - **Mac/Linux:** use the normal Terminal app.
2. Get the installer:

   ```sh
   git clone https://github.com/xanstomper/Tiramisu.git
   cd Tiramisu
   ```

3. Copy `Tiramisu.msix` and `dev.cer` into this folder.
4. Run the installer (replace the IP, username, and password with yours):

   ```sh
   export DP_HOST=10.0.0.23
   export DP_USER=your_portal_user
   export DP_PASS=your_portal_pass
   bash tools/deploy.sh install ./Tiramisu.msix ./dev.cer
   ```

   A good result ends with `INSTALL OK`.

## Step 5 — Launch Tiramisu

1. On the Xbox, open **Dev Home → My games & apps**.
2. Find **Tiramisu** and press **A**.
3. You should see the Tiramisu menu with a QR code and a web address.

## Step 6 — Put games on the Xbox

1. Scan the QR code on the TV with your phone, or open
   `http://<xbox-ip>:9127` on your computer (use `http`, not `https`).
2. Add your game:
   - **Computer:** drag the game folder onto the page, or upload a `.zip` of it.
   - **Phone:** zip the game folder first (phone browsers cannot choose whole
     folders), then tap **Upload .zip**.
3. Wait for the upload to finish, then press **Launch** (or A on the
   controller).

To make a `.zip`: on Windows, right-click the game folder → **Send to →
Compressed (zipped) folder**. On Mac, right-click → **Compress**.

## Why is there a `dev.cer` file?

Xbox apps must be signed. Tiramisu is signed with its own self-signed
certificate because it is not a Store app. `dev.cer` is that certificate — it
tells the Xbox "trust the signature on this app."

It is not a game, not a dependency, and not something you open. The
command-line installer sends it with the `.msix`, which is why that method
works even though the portal website has no Certificate box. After the
certificate is trusted once, updates signed with the same certificate install
without needing it again.

## Updates and saves

- **Back up saves before updating:** open the LAN dashboard → **Saves &
  Support → Backup Saves (.zip)**. Uninstalling a Dev Mode app wipes its saved
  data.
- **To update:** install the new `Tiramisu.msix` (Step 4). If the portal says
  the same version is already installed, uninstall the old app first, then
  install the new one.

## Troubleshooting

| Problem | What to do |
|---|---|
| Browser warns about the site | Normal. Advanced → Continue. |
| Web install fails with a signature error | Use the command-line installer (Step 4). |
| Error `0x8007000B` | Wrong pair. `Tiramisu.msix` needs `dev.cer`; KBM needs `edge-dev.cer`. |
| Tiramisu opens then closes instantly | Make sure "Treat UWP apps as games by default" is ON (Step 2). |
| LAN page (`:9127`) does not load | Same Wi-Fi as the Xbox, and use `http://` (not `https://`). |

More problems: see [troubleshooting.md](troubleshooting.md).

## Quick reference

```text
Device Portal:  https://<xbox-ip>:11443
LAN dashboard:  http://<xbox-ip>:9127
Normal pair:    Tiramisu.msix  + dev.cer
KBM pair:       Tiramisu-EdgeSpoof.msix + edge-dev.cer
Installer:      bash tools/deploy.sh install <msix> <cer>
Memory setting: Dev Home → Settings → "Treat UWP apps as games by default" = ON
```
