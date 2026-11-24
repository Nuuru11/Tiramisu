# Install Tiramisu on your Xbox — beginner guide

This guide is for someone who has **never used Developer Mode before**. It
explains every step in plain language.

## What you need

- An Xbox One, One S, One X, Series S or Series X.
- A phone or computer on the **same Wi-Fi network** as the Xbox.
- About 10 minutes.
- Optional: a terminal (Git Bash/WSL on Windows, Terminal on Mac/Linux) if the
  web install fails. This is the "command-line installer" described in Step 7.

---

## Step 1 — Put the Xbox into Developer Mode

1. On the Xbox, open the **Microsoft Store**.
2. Install the app called **Dev Mode** (some people call it Xbox Dev Mode).
3. Open it and sign in with a Microsoft account.
4. Choose **Switch to Dev Mode**. The Xbox will restart.
5. After the restart, open **Dev Home**. This is the home screen you will use
   while the console is in Dev Mode.

---

## Step 2 — Find the Xbox IP address

1. In **Dev Home**, look on the **Network** page (or on the home screen).
2. You will see an address like `10.0.0.23`. Write it down.
3. The Device Portal website is that address with `:11443` on the end:

   `https://10.0.0.23:11443`

---

## Step 3 — Turn on the memory setting

1. In **Dev Home**, go to **Settings**.
2. Turn **ON** the option called **"Treat UWP apps as games by default"**.
3. This gives Tiramisu enough memory to run games. If you skip it, Tiramisu can
   close immediately after opening.

---

## Step 4 — Download the two files

On your phone or computer, open:

https://github.com/xanstomper/Tiramisu/releases

Under the newest release (**v0.2.4.12 or later**), download:

- `Tiramisu.msix` — the app
- `dev.cer` — the signing certificate

Keep them together in the same folder.

---

## Step 5 — Open the Xbox website

1. In a browser on your phone or computer (Edge, Chrome or Safari), type the
   address from Step 2:

   `https://10.0.0.23:11443`

2. The browser will warn that the site is not secure. This is **normal** for an
   Xbox in Dev Mode. Tap **Advanced → Continue** (or **Proceed anyway**).
3. Sign in with the Dev Home account if it asks.

---

## Step 6 — Install through the web page (try this first)

1. On the website, go to **My games & apps → Add**.
2. In the **App package** box, choose **`Tiramisu.msix`**.
3. Leave **Dependencies** empty.
4. Click **Add** and wait until the page says the install finished.

**There is no Certificate box on current Xbox Device Portal.** That is normal.
Do not put `dev.cer` in Dependencies.

If it says the install succeeded, go to **Step 8**.

If it fails with a signature/error message, do **Step 7**.

---

## Step 7 — The command-line installer (reliable method)

The command-line installer is a script from the Tiramisu repo called
`tools/deploy.sh`. It logs into your Xbox, sends `Tiramisu.msix` **and**
`dev.cer` together, and waits for the install to finish. The certificate is
passed automatically, so you never need a Certificate box.

### Setup (one time)

You need a terminal with `curl` and `python3`:

- **Windows:** install **Git Bash** or **WSL**
- **Mac/Linux:** use the normal Terminal

Download the repo:

```sh
git clone https://github.com/xanstomper/Tiramisu.git
cd Tiramisu
```

Put `Tiramisu.msix` and `dev.cer` somewhere you can point to. For example, copy
both files into the `Tiramisu` folder you just cloned.

Find your Device Portal username and password on the Xbox:
**Dev Home → Settings → Device Portal**. (This is the same account/password you
use to sign into the website in Step 5.)

### Run the installer

In the terminal, from the `Tiramisu` folder:

```sh
export DP_HOST=10.0.0.23          # your Xbox IP from Step 2
export DP_USER=your_portal_user   # from Dev Home → Settings → Device Portal
export DP_PASS=your_portal_pass

bash tools/deploy.sh install ./Tiramisu.msix ./dev.cer
```

If the files are elsewhere, use their full paths, for example:

```sh
bash tools/deploy.sh install /Users/me/Downloads/Tiramisu.msix /Users/me/Downloads/dev.cer
```

A successful install looks like:

```text
upload HTTP 202
{"Reason" : "Deploy request accepted and being processed"}
state: {"Code" : 0, "CodeText" : "The operation completed successfully.", "Success" : true}
INSTALL OK
```

---

## Step 8 — Launch Tiramisu

1. Go back to the Xbox and open **Dev Home**.
2. Open **My games & apps**.
3. Find **Tiramisu** and press **A** to launch it.

You should see the Tiramisu menu with a QR code and a web address.

---

## Step 9 — Put a game on the Xbox

1. On the Tiramisu screen, scan the QR code with your phone, or open the web
   address shown on the TV (it looks like `http://10.0.0.23:9127`).
2. **On a phone:** zip your game folder first, then tap **Upload .zip**. Phone
   browsers cannot select whole folders.
3. **On a computer:** drag the game folder onto the page.
4. When the game appears in the list, press **Launch** (or A on the controller).

---

## Why is `dev.cer` necessary?

Xbox Dev Mode apps must be **signed**. We do not have a Microsoft Store signing
key, so Tiramisu is signed with our own **self-signed certificate**. `dev.cer`
is that certificate. It tells the Xbox "trust the signature on this app".

It is **not** a game, **not** a dependency and **not** an app you open. The
command-line installer sends it to the Xbox together with the `.msix`, which is
why Step 7 always works even though the web page has no Certificate box.

---

## If something goes wrong

| Problem | What to do |
|---|---|
| Browser warns about the site | Normal. Tap Advanced → Continue. |
| Web install fails with a signature error | Use Step 7, the command-line installer. |
| Error `0x8007000B` | Wrong certificate pair. Use `dev.cer` with `Tiramisu.msix`. |
| Tiramisu opens then closes instantly | Make sure "Treat UWP apps as games by default" is ON (Step 3). |
| Phone will not upload a folder | Zip the folder first and use Upload .zip. |
| Game does not start | Check the compatibility badge. 32-bit x86 and D3D12-only games may not work yet. |

---

## Want to use a real mouse and keyboard?

The normal build uses the controller or the phone trackpad. The release also
includes a **KBM edition** (`Tiramisu-EdgeSpoof.msix` + `edge-dev.cer`) that
tries to unlock the Xbox's hardware mouse support. It is experimental. If you
use it, pair it only with `edge-dev.cer` and read `XBOX-MOUSE.md` first.
