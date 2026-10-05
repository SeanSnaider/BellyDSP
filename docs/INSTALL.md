# Installing BellyDSP

BellyDSP is a guitar amp and effects app. You plug your guitar into an audio interface, and it plays through amp captures, a cab, pedals, and effects. Here's how to get it running. It takes about five minutes, and after that it keeps itself up to date.

You need: an audio interface (anything with an instrument input, like a Focusrite Scarlett), headphones or speakers on that interface, and a Mac with macOS 12 or later (Apple silicon or Intel) or a Windows 10 or 11 PC.

Downloads: **https://github.com/SeanSnaider/BellyDSP/releases/latest**

## Mac

1. **Download** `BellyDSP-<version>.dmg` from the link above and open it.
2. **Drag BellyDSP onto the Applications folder** in the window that opens. (Run it from Applications, not from the disk image or Downloads, or it can't update itself.)
3. **Open it the first time.** macOS will say it "can't verify" BellyDSP, because it's free software from an independent developer rather than an app notarized by Apple. That's expected:
   - Click **Done** (not Move to Trash).
   - Open **System Settings > Privacy & Security**, scroll down to the line about BellyDSP, and click **Open Anyway**. Confirm with your password or Touch ID.
   - Open BellyDSP again and click **Open**.

   If you're comfortable with Terminal, this one line does the same thing:
   ```
   xattr -dr com.apple.quarantine "/Applications/BellyDSP.app"
   ```
   You only do this once. Updates don't ask again.
4. **Allow the microphone** when BellyDSP asks. That's how macOS names any audio input, including your guitar. (Changed your mind? System Settings > Privacy & Security > Microphone.)
5. **Set your interface to 48 kHz.** Open **Audio MIDI Setup** (Applications > Utilities), click your interface, and set Format to **48,000 Hz**. BellyDSP only runs at 48 kHz; at any other rate it stays silent and tells you so.
6. **Choose the input** (see "Audio settings" below).

## Windows

1. **Download** `BellyDSP-<version>-windows-setup.exe` from the link above and run it.
2. Windows may show **"Windows protected your PC"** (the installer isn't signed with a paid certificate). Click **More info**, then **Run anyway**.
3. Click through the installer. It installs just for you, so it doesn't need an administrator password.
4. **Set your interface to 48 kHz** in its own control panel (or Settings > Sound > your device > Properties > Advanced). BellyDSP only runs at 48 kHz.
5. **Choose the input** (below). For the lowest delay, pick the device type **ASIO** and your interface's own ASIO driver (for a Focusrite Scarlett on USB: **Focusrite USB ASIO**; install the interface maker's driver if it isn't listed). Without an ASIO driver, use **Windows Audio (Exclusive Mode)** and the smallest buffer size that plays without crackles (often 256 or more; it may not offer 128).

## Audio settings (both)

Click **Options** at the top of BellyDSP's window, then **Audio/MIDI Settings**:

- **Output** and **Input**: your audio interface. **Sample rate**: 48000.
- **Active input channels**: only the one your guitar is plugged into (usually Input 1).
- **Buffer size**: 128 samples is a good start. Smaller feels tighter; if you hear crackles, go up to 256.
- **Untick "Mute audio input"**. It starts ticked (a safety against feedback), and until you untick it you'll hear nothing. A yellow bar at the top says so.

On the interface itself: turn the instrument (INST or Hi-Z) switch on for the guitar input, turn that input's **gain knob all the way down**, and turn **Direct Monitor off** (otherwise you hear your dry guitar on top of the amp).

Why minimum gain: BellyDSP calibrates its amps to your interface's level at minimum gain, so your guitar hits each amp the way it would hit the real thing. Turning the interface gain up is like putting a boost pedal in front of every amp: more distortion, harsher, and clipping on hard strums. Get distortion from the amp's **Gain** knob instead, and volume from **Master** or the Output knob. If your interface isn't a Scarlett Solo 4th Gen, open the **Input** page and set **Interface level at 0 dBFS** to the most your instrument input takes at minimum gain, in dBu (it's on the interface's spec sheet; +12 dBu is the Solo's). If you can't find it, leave it at 12.

## Captures and cab IRs

BellyDSP uses NAM captures (`.nam` files) for its amps and impulse responses (`.wav` files) for the cab. Free ones are all over the place (TONE3000 has thousands of captures; pick "amp only" ones, since BellyDSP adds its own cab).

Keep them here, so presets can find them again even if you rename things:

| | Mac | Windows |
|---|---|---|
| Captures | `~/Library/Application Support/BellyDSP/models` | `%APPDATA%\BellyDSP\models` |
| Cab IRs | `~/Library/Application Support/BellyDSP/irs` | `%APPDATA%\BellyDSP\irs` |

(Mac: in Finder, Go > Go to Folder... and paste the path. Windows: paste the path into File Explorer's address bar. Make the folders if they're not there yet.)

Then click an amp's grille on the Amp page to load a capture, and pick a cab on the Cab page.

## Updates

They happen by themselves:

- **Mac**: once a day BellyDSP checks for a new version, downloads it quietly, and installs it the next time you quit. Nothing to click.
- **Windows**: once a day it checks, and when there's a new version it asks. Click **Install update**: it closes, updates, and opens again.

To see which version you have, or to check right now, click **BellyDSP** at the top-left of the window.

## Something's wrong

| What you notice | Try |
|---|---|
| No sound at all | "Mute audio input" still ticked in Options; the guitar's input channel not enabled; the microphone permission denied (Mac); the interface not at 48 kHz |
| You hear your dry guitar too | Turn Direct Monitor off on the interface |
| Crackles | A bigger buffer size in Options |
| Mac says BellyDSP "is damaged" or can't be opened | Do the first-launch steps above (or the `xattr` line) |
| Mac never updates | Make sure BellyDSP is in Applications, and quit it now and then |

## Licence

BellyDSP is free software: Copyright (C) 2026 Sean Snaider, under the GNU Affero General Public License, version 3 or later. It comes with ABSOLUTELY NO WARRANTY. The source code is at https://github.com/SeanSnaider/BellyDSP, and **BellyDSP** (top-left) > **Source code for this version** opens the exact source of the copy you're running. Licences for everything BellyDSP includes: **BellyDSP** > **About / licenses** (also `THIRD_PARTY_NOTICES.txt` and `LICENSE.txt` in the download).

**Coming from Amp Sim** (the app's old name)? The first time BellyDSP starts, it copies your presets, captures, IRs, and audio settings from the old Amp Sim folders to BellyDSP's. The old folders stay exactly as they were; delete them yourself once you're happy.
