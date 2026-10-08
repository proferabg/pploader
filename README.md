# Peer Pressure Plugin Loader [![Github All Releases](https://img.shields.io/github/downloads/proferabg/pploader/total.svg)]()

Just a simple plugin loader + installer for sysdll plugin loading on Peer Pressure

# Install

1. Install [Peer Pressure](https://github.com/grimdoomer/Xbox360PeerPressure)
2. Download the [Latest Release](https://github.com/proferabg/pploader/releases/latest)
3. Extract the Content folder onto the root of a USB or HDD and launch `PPLoader Installer` on the Xbox 360

# Usage

It will load plugins contained in pploader.ini from HDD or USB root.

# Known Bugs

1. XBDM has been patched to load without Dashlaunch and is not fully working, this will be addressed in the future.
2. JRPC2 has a race condition and will sometimes halt the console on load, use XDRPC instead.

# Persistence

This will only persist until Peer Pressure runs an update, in which case, you will need to run the installer again.

# Disclaimer

This is only for research and development. I do not condone online hacking, cheating, or piracy of any kind. 

PPLoader is also NOT affiliated with GrimDoomer or Peer Pressure in any shape or form and is simply a separate 3rd-party application.

# FAQs

1. Why doesn't 'x' plugin work?
    * I don't know. Contact the creator of the plugin.

2. Can this load stealth servers?
    * This is simply a plugin loader. I can't control what this is used for. I do however don't condone the idea of loading stealth servers with PPLoader.

3. Is this a Dashlaunch alternative?
    * No, this is just a plugin loader.

4. Will this receive updates?
    * Yes, but on my own time. You are welcome to submit pull requests.

5. Why doesn't this have 'x' feature?
    * This is just a plugin loader, nothing else.

# License Notice

The Xbox 360 loader and installer are covered by the repository's root license.

The `pkgbuilder` component is distributed under the GNU General Public License version 3. It builds DJ SkunkieButt's GPL-3.0 X360 .NET library from the `pkgbuilder/X360` Git submodule at `https://github.com/mtolly/X360`; no prebuilt `X360.dll` is stored in this repository. A small reproducible compatibility patch is applied only while building X360 and the submodule is restored afterward. Its complete license and attribution are in `pkgbuilder/LICENSE-GPL-3.0.txt` and `pkgbuilder/NOTICE.md`.
