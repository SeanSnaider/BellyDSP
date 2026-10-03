# Installing Amp Sim

Amp Sim is a guitar amp and effects app. You plug your guitar into an audio interface, and it plays through amp captures, a cab, pedals, and effects. Here's how to get it running. It takes about five minutes, and after that it keeps itself up to date.

You need: an audio interface (anything with an instrument input, like a Focusrite Scarlett), headphones or speakers on that interface, and a Mac with macOS 12 or later (Apple silicon or Intel) or a Windows 10 or 11 PC.

Downloads: **https://github.com/SeanSnaider/amp-sim-releases/releases/latest**

## Mac

1. **Download** `AmpSim-<version>.dmg` from the link above and open it.
2. **Drag Amp Sim onto the Applications folder** in the window that opens. (Run it from Applications, not from the disk image or Downloads, or it can't update itself.)
3. **Open it the first time.** macOS will say it "can't verify" Amp Sim, because it's a friend's app rather than one registered with Apple. That's expected:
   - Click **Done** (not Move to Trash).
   - Open **System Settings > Privacy & Security**, scroll down to the line about Amp Sim, and click **Open Anyway**. Confirm with your password or Touch ID.
   - Open Amp Sim again and click **Open**.

   If you're comfortable with Terminal, this one line does the same thing:
   ```
   xattr -dr com.apple.quarantine "/Applications/Amp Sim.app"
   ```
   You only do this once. Updates don't ask again.
4. **Allow the microphone** when Amp Sim asks. That's how macOS names any audio input, including your guitar. (Changed your mind? System Settings > Privacy & Security > Microphone.)
5. **Set your interface to 48 kHz.** Open **Audio MIDI Setup** (Applications > Utilities), click your interface, and set Format to **48,000 Hz**. Amp Sim only runs at 48 kHz; at any other rate it stays silent and tells you so.
6. **Choose the input** (see "Audio settings" below).

## Windows

1. **Download** `AmpSim-<version>-windows-setup.exe` from the link above and run it.
2. Windows may show **"Windows protected your PC"** (the installer isn't signed with a paid certificate). Click **More info**, then **Run anyway**.
3. Click through the installer. It installs just for you, so it doesn't need an administrator password.
4. **Set your interface to 48 kHz** in its own control panel (or Settings > Sound > your device > Properties > Advanced). Amp Sim only runs at 48 kHz.
5. **Choose the input** (below). For the lowest delay, pick the device type **Windows Audio (Exclusive Mode)**.

## Audio settings (both)

Click **Options** at the top of Amp Sim's window, then **Audio/MIDI Settings**:

- **Output** and **Input**: your audio interface. **Sample rate**: 48000.
- **Active input channels**: only the one your guitar is plugged into (usually Input 1).
- **Buffer size**: 128 samples is a good start. Smaller feels tighter; if you hear crackles, go up to 256.
- **Untick "Mute audio input"**. It starts ticked (a safety against feedback), and until you untick it you'll hear nothing. A yellow bar at the top says so.

On the interface itself: turn the instrument (INST or Hi-Z) switch on for the guitar input, set the gain so your hardest strum stays out of the red, and turn **Direct Monitor off** (otherwise you hear your dry guitar on top of the amp).

## Captures and cab IRs

Amp Sim uses NAM captures (`.nam` files) for its amps and impulse responses (`.wav` files) for the cab. Free ones are all over the place (TONE3000 has thousands of captures; pick "amp only" ones, since Amp Sim adds its own cab).

Keep them here, so presets can find them again even if you rename things:

| | Mac | Windows |
|---|---|---|
| Captures | `~/Library/Application Support/AmpSim/models` | `%APPDATA%\AmpSim\models` |
| Cab IRs | `~/Library/Application Support/AmpSim/irs` | `%APPDATA%\AmpSim\irs` |

(Mac: in Finder, Go > Go to Folder... and paste the path. Windows: paste the path into File Explorer's address bar. Make the folders if they're not there yet.)

Then click an amp's grille on the Amp page to load a capture, and pick a cab on the Cab page.

## Updates

They happen by themselves:

- **Mac**: once a day Amp Sim checks for a new version, downloads it quietly, and installs it the next time you quit. Nothing to click.
- **Windows**: once a day it checks, and when there's a new version it asks. Click **Install update**: it closes, updates, and opens again.

To see which version you have, or to check right now, click **rig** at the top-left of the window.

## Something's wrong

| What you notice | Try |
|---|---|
| No sound at all | "Mute audio input" still ticked in Options; the guitar's input channel not enabled; the microphone permission denied (Mac); the interface not at 48 kHz |
| You hear your dry guitar too | Turn Direct Monitor off on the interface |
| Crackles | A bigger buffer size in Options |
| Mac says Amp Sim "is damaged" or can't be opened | Do the first-launch steps above (or the `xattr` line) |
| Mac never updates | Make sure Amp Sim is in Applications, and quit it now and then |

Licences for everything Amp Sim includes: click **rig** > **About / licenses**.
