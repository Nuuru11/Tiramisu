# Tiramisu — full beginner install guide

This is the complete guide for installing Tiramisu on an Xbox in Developer Mode,
transferring games, and fixing the common problems. It assumes you have never
used Dev Mode before.

---

## Contents

1. [What you need](#what-you-need)
2. [Step 1 — Put the Xbox into Developer Mode](#step-1--put-the-xbox-into-developer-mode)
3. [Step 2 — Find the Xbox IP address](#step-2--find-the-xbox-ip-address)
4. [Step 3 — Turn on the memory setting](#step-3--turn-on-the-memory-setting)
5. [Step 4 — Download the files](#step-4--download-the-files)
6. [Step 5 — Open the Xbox website](#step-5--open-the-xbox-website)
7. [Step 6 — Install from the website](#step-6--install-from-the-website)
8. [Step 7 — Command-line installer (if the website fails)](#step-7--command-line-installer-if-the-website-fails)
9. [Step 8 — Launch Tiramisu](#step-8--launch-tiramisu)
10. [Step 9 — Put games on the Xbox](#step-9--put-games-on-the-xbox)
11. [Why is there a `dev.cer` file?](#why-is-there-a-devcer-file)
12. [Updates and saves](#updates-and-saves)
13. [Keyboard and mouse](#keyboard-and-mouse)
14. [Troubleshooting](#troubleshooting)
15. [Getting help](#getting-help)

---

## What you need

- Xbox One, One S, One X, Series S or Series X.
- The Xbox and your phone/computer on the **same Wi-Fi**.
- A Microsoft account for Dev Mode.
- A browser (Edge, Chrome or Safari).
- Optional: a terminal with `curl` and `python3` (Git Bash/WSL on Windows) for
  the command-line installer.

---

## Step 1 — Put the Xbox into Developer Mode

1. On the Xbox, open the **Microsoft Store**.
2. Search for and install **Dev Mode** (sometimes called Xbox Dev Mode).
3. Open the app, sign in with a Microsoft account.
4. Choose **Switch to Dev Mode**. The console restarts.
5. After the restart, open **Dev Home**. This is your Dev Mode home screen.

---

## Step 2 — Find the Xbox IP address

1. In **Dev Home**, look at the home screen or the **Network** page.
2. Write down the IP address, for example `10.0.0.23`.
3. The Device Portal address is `https://<your-ip>:11443`.

If you cannot find it, press the Xbox button → **Settings → Network →
Advanced settings** (wording varies by OS build) and look for the IP address.

---

## Step 3 — Turn on the memory setting

1. In **Dev Home**, open **Settings**.
2. Turn **ON** — **"Treat UWP apps as games by default"**.
3. This gives Tiramisu 5 GB of memory. Without it, the app can close straight
   away, especially on Xbox One S.

---

## Step 4 — Download the files

On your phone or computer, open:

https://github.com/xanstomper/Tiramisu/releases

Under the newest release (v0.2.4.12 or later), download:

| File | What it is |
|---|---|
| `Tiramisu.msix` | The app itself |
| `dev.cer` | The signing certificate |

Keep them together in one folder (for example a folder called `Tiramisu`).

For keyboard/mouse (optional), download instead:

| File | What it is |
|---|---|
| `Tiramisu-EdgeSpoof.msix` | Experimental KBM version of the app |
| `edge-dev.cer` | Its matching certificate |

Do not mix the two pairs.

---

## Step 5 — Open the Xbox website

1. In your browser, type the address from Step 2, e.g.:

   `https://10.0.0.23:11443`

2. The browser warns that the connection is not secure. This is normal. Choose
   **Advanced → Continue** (or **Proceed anyway**).
3. Sign in using the Dev Home / Device Portal account. It is the account you
   set in **Dev Home → Settings → Device Portal**.

---

## Step 6 — Install from the website

Try this first. It works on many consoles.

1. On the Device Portal website, go to **My games & apps → Add**.
2. In **App package**, choose `Tiramisu.msix`.
3. Leave **Dependencies** empty.
4. Click **Add**.
5. Wait for the page to say the install finished.

**There is no Certificate box on current Xbox Device Portal.** Do not look for
one and do not put `dev.cer` in Dependencies. If the install fails, go to
Step 7.

---

## Step 7 — Command-line installer (if the website fails)

This is the reliable method. The script `tools/deploy.sh` logs into Device
Portal, uploads `Tiramisu.msix` **and** `dev.cer` together, and waits for the
result. The certificate is handled for you.

### 7.1 Install a terminal (one time)

- **Windows:** install **Git for Windows** from https://git-scm.com (gives you
  "Git Bash"), or run `wsl --install` and use Ubuntu.
- **Mac/Linux:** use the normal Terminal app.

Check the tools exist:

```sh
curl --version
python3 --version
```

### 7.2 Get the installer

```sh
git clone https://github.com/xanstomper/Tiramisu.git
cd Tiramisu
```

Copy `Tiramisu.msix` and `dev.cer` into this folder (or remember their full
paths).

### 7.3 Find your Device Portal login

On the Xbox: **Dev Home → Settings → Device Portal**. It shows the username
and password used to sign into the website. Set them if you have not already.

### 7.4 Run the installer

```sh
export DP_HOST=10.0.0.23          # your Xbox IP
export DP_USER=your_portal_user
export DP_PASS=your_portal_pass

bash tools/deploy.sh install ./Tiramisu.msix ./dev.cer
```

If the files are elsewhere:

```sh
bash tools/deploy.sh install /path/to/Tiramisu.msix /path/to/dev.cer
```

A good result looks like:

```text
upload HTTP 202
{"Reason" : "Deploy request accepted and being processed"}
state: {"Code" : 0, "CodeText" : "The operation completed successfully.", "Success" : true}
INSTALL OK
```

Optional: test the connection first with
`bash tools/deploy.sh info`.

---

## Step 8 — Launch Tiramisu

1. On the Xbox, open **Dev Home**.
2. Open **My games & apps**.
3. Find **Tiramisu** and press **A**.
4. You should see the Tiramisu menu with a QR code and a web address.

Check that the dashboard is alive by opening `http://<xbox-ip>:9127/api/status`
on your PC/phone. It should show `"running":true`.

---

## Step 9 — Put games on the Xbox

1. Scan the QR code on the TV with your phone, or open the address shown on the
   TV (`http://<xbox-ip>:9127`) on your computer.
2. Add your game:
   - **Phone:** zip the game folder first, then tap **Upload .zip**. Phone
     browsers cannot choose whole folders.
   - **Computer:** drag the game folder onto the page, or use Upload .zip.
3. Wait for the upload to finish.
4. In the game list, choose the `.exe` if asked, then press **Launch** (or A on
   the controller).

### How to make a .zip

- **Windows:** right-click the game folder → **Send to → Compressed (zipped)
  folder**.
- **Mac:** right-click → **Compress**.
- **iPhone/Android:** use a file manager app to compress the folder, or upload
  the individual files with **Pick Files** instead.

---

## Why is there a `dev.cer` file?

Xbox Dev Mode apps must be **signed**. We do not have a Microsoft Store signing
key, so Tiramisu is signed with our own **self-signed certificate**. `dev.cer`
is that certificate. It means "trust the signature on this app".

It is **not** a game, **not** a dependency and **not** an app you open. The
command-line installer sends it with the `.msix`, which is why Step 7 works
even though the web page has no Certificate box.

---

## Updates and saves

- Back up saves **before** updating: open the LAN dashboard → **Saves & Support
  → Backup Saves (.zip)**. Uninstalling a Dev Mode app wipes its LocalState.
- To update: install the new `Tiramisu.msix` (Step 6 or 7). If the portal says
  the same version is already installed, uninstall the old app first, then
  install the new one.
- Restoring saves: **Saves & Support → Restore Saves (.zip)**.

---

## Keyboard and mouse

The normal build uses the controller (`pad2kbm`: right stick moves the mouse,
triggers click) or the phone trackpad at `/controls`.

The experimental **KBM edition** (`Tiramisu-EdgeSpoof.msix` + `edge-dev.cer`)
tries to enable real hardware mouse passthrough. Install it with:

```sh
bash tools/deploy.sh install ./Tiramisu-EdgeSpoof.msix ./edge-dev.cer
```

It uses a different package identity, so its games/saves start in a fresh
LocalState. Read `XBOX-MOUSE.md` before using it.

---

## Troubleshooting

| Problem | What to do |
|---|---|
| Browser warns about the site | Normal. Advanced → Continue. |
| Web install fails with signature error | Use Step 7, the command-line installer. |
| Error `0x8007000B` | Wrong pair. `Tiramisu.msix` needs `dev.cer`; KBM needs `edge-dev.cer`. |
| Error `0x80073CF3` | A package with that identity is already registered. Uninstall the old app first. |
| Tiramisu opens then closes instantly | Make sure "Treat UWP apps as games by default" is ON (Step 3). |
| Phone will not upload a folder | Zip it first and use Upload .zip. |
| Game does not start | Check the compatibility badge. 32-bit x86 and D3D12-only games may not work yet. |
| LAN page does not load | Make sure the phone/PC is on the same Wi-Fi and use `http://<xbox-ip>:9127` (not https). |

---

## Getting help

1. Open the LAN dashboard (`http://<xbox-ip>:9127`) → **Saves & Support →
   Download Debug Bundle (.zip)**. This gathers the log, settings and crash
   info.
2. Or pull the log with:

   ```sh
   bash tools/deploy.sh info-file '<full-package-name>' 'LocalState/tiramisu.log'
   ```

3. Post the last 50 lines of the log when asking for help.

---

## Quick reference

```text
Device Portal:  https://<xbox-ip>:11443
LAN dashboard:  http://<xbox-ip>:9127
Normal pair:    Tiramisu.msix  + dev.cer
KBM pair:       Tiramisu-EdgeSpoof.msix + edge-dev.cer
Installer:      bash tools/deploy.sh install <msix> <cer>
Required setting: Dev Home → Settings → "Treat UWP apps as games by default" = ON
```
