# PPLoader package builder licensing

The `pkgbuilder` component is distributed under the GNU General Public License version 3. See `LICENSE-GPL-3.0.txt` for the complete license text.

`PPLoaderPkgBuilder` uses DJ SkunkieButt's X360 .NET library to create and verify STFS packages. X360 is included as the `pkgbuilder/X360` Git submodule from `https://github.com/mtolly/X360` and is built from source as part of the package-builder build. The submodule is pinned by Git; its upstream license and notices remain available within the submodule.

X360 is also distributed under GNU GPL version 3. The currently pinned upstream revision is `573dda3cd841ba370b2110567a6b4ee2b7099c9c`.

The local build applies `patches/X360-pkgbuilder.patch` temporarily and restores the submodule afterward. The patch corrects exact-4-KiB STFS file writes and removes two obsolete web privilege-check calls from the package creation and open paths. The patch is part of the GPL-covered `pkgbuilder` source.
