# Install Tiramisu on your Xbox — beginner guide

This guide is for someone who has never used Developer Mode before.

## What you need

- An Xbox One, One S, One X, Series S or Series X in **Developer Mode**.
- A phone or computer on the same Wi-Fi as the Xbox.
- About 5 minutes.

## Step 1 — Put the Xbox in Developer Mode

1. On the Xbox, open the **Microsoft Store** and install the app called **Dev Mode** (some people call it Xbox Dev Mode).
2. Open it and sign in with a Microsoft account.
3. Choose **Switch to Dev Mode**. The Xbox will restart.
4. After the restart, open **Dev Home**. This is your new home screen while in Dev Mode.

## Step 2 — Find the Xbox address

1. In **Dev Home**, look on the **Network** page (or the home screen).
2. You will see an address like `10.0.0.23`. Write it down.

## Step 3 — Turn on the memory setting

1. In **Dev Home**, go to **Settings**.
2. Turn **ON** the option called **"Treat UWP apps as games by default"**.
3. This gives Tiramisu enough memory to run games. If you skip this, Tiramisu may close straight away.

## Step 4 — Download the two files

On your phone or computer, open:

https://github.com/xanstomper/Tiramisu/releases

Under the newest release (v0.2.4.12 or later), download these two files:

- `Tiramisu.msix`
- `dev.cer`

Keep them together in the same folder.

## Step 5 — Open the Xbox website

1. On your phone or computer, open a browser (Edge, Chrome or Safari).
2. Type this in the address bar, using your Xbox address from Step 2:

   `https://10.0.0.23:11443`

3. The browser will show a warning. This is normal for an Xbox in Dev Mode.
   Tap **Advanced** → **Continue** (or **Proceed anyway**).
4. Sign in with the same account you used in Dev Home, if it asks.

## Step 6 — Add the app

1. On the website, go to **My games & apps** → **Add**.
2. In the **App package** box, choose **`Tiramisu.msix`**.
3. Leave **Dependencies** empty.
4. Click **Add** and wait until the page says the install finished.

**You will not see a Certificate box — that is normal.** Current Xbox Device
Portal builds do not show one. Do not put `dev.cer` in Dependencies.

### Why is there a `dev.cer` file at all?

Apps for Xbox Dev Mode must be **signed**. We do not have a Microsoft Store
signing key, so the release is signed with our own self-signed certificate.
`dev.cer` is that certificate. It tells the Xbox "trust the signature on this
app". It is **not** a game, **not** a dependency and **not** an app you install
by itself.

The command-line installer passes the `.cer` to the Xbox automatically. The web
page does not have a field for it, so if the web install fails with a signature
error, use the installer below.

### If the web install fails — the reliable method

On a Windows PC with Git Bash or WSL (or any Linux/Mac terminal):

```sh
git clone https://github.com/xanstomper/Tiramisu.git
cd Tiramisu
export DP_HOST=10.0.0.23          # your Xbox IP from Step 2
export DP_USER=your_portal_user   # from Dev Home → Settings → Device Portal
export DP_PASS=your_portal_pass
bash tools/deploy.sh install /path/to/Tiramisu.msix /path/to/dev.cer
```

The command sends both files to the Xbox in one request and waits for success.

## Step 7 — Launch Tiramisu

1. Go back to the Xbox and open **Dev Home**.
2. Open **My games & apps**.
3. Find **Tiramisu** and press **A** to launch it.

You should see the Tiramisu menu screen with a QR code and a web address.

## Step 8 — Put a game on the Xbox

1. On the Tiramisu screen, scan the QR code with your phone, or open the address shown on the TV (it looks like `http://10.0.0.23:9127`).
2. **On a phone:** zip your game folder first, then tap **Upload .zip**. Phone browsers cannot pick whole folders.
3. **On a computer:** drag the game folder onto the page.
4. When it appears in the game list, press **Launch** (or A on the controller).

## If something goes wrong

| Problem | What to do |
|---|---|
| Browser warns about the site | Normal. Tap Advanced → Continue. |
| Install says `0x8007000B` | You used the wrong certificate. Use `dev.cer` with `Tiramisu.msix`. |
| Tiramisu closes immediately | Make sure "Treat UWP apps as games by default" is ON in Dev Home Settings. |
| Phone will not upload a folder | Zip the folder first and use Upload .zip. |
| Game does not start | Check the compatibility badge next to the game name. 32-bit x86 and D3D12-only games may not work yet. |

## Want to use a real mouse and keyboard?

The normal build uses the controller or the phone trackpad. The release also
includes a **KBM edition** (`Tiramisu-EdgeSpoof.msix` + `edge-dev.cer`) that
tries to unlock the Xbox's hardware mouse support. It is experimental. If you
use it, pair it only with `edge-dev.cer` and read `XBOX-MOUSE.md` first.
