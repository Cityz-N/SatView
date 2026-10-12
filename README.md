SatView - aerial imagery layer for EuroScope (France)
Imagery (c) IGN - BD ORTHO, from the open IGN Geoplateforme service. No account, no key.

WHAT YOU GET
  Aerial photos behind/over your radar picture, at the detail that matches your zoom.
  Tiles are loaded as you look around and kept in a cache folder (default limit 8 GB; the
  oldest tiles are deleted first). Nothing is downloaded in bulk.

HOW TO BUILD (same as WxOverlay, one time)
 1. Visual Studio 2022 with "Desktop development with C++" and the MFC component.
 2. New project: "MFC DLL", name it SatView, "Regular DLL using shared MFC DLL".
 3. Platform x86, configuration Release.
 4. Replace the whole generated SatView.cpp with the SatView.cpp from this folder; remove
    the other generated files (pch.h/pch.cpp/framework.h/SatView.h/.def/.rc/.rc2/Resource.h).
 5. Project properties (Release, x86):
      - C/C++ > Precompiled Headers > Not Using Precompiled Headers
      - C/C++ > General > Additional Include Directories > folder with EuroScopePlugIn.h
      - Linker > General > Additional Library Directories > folder with EuroScopePlugInDll.lib
      - General > Use of MFC > Use MFC in a Static Library
      - C/C++ > Code Generation > Runtime Library > Multi-threaded (/MT)
 6. Build. Copy Release\SatView.dll and SatView.ini into the same folder (your Plugins folder).
 7. EuroScope: Other SET > Plug-ins > Load > SatView.dll.
    Tip: load SatView BEFORE WxOverlay so that the weather is drawn on top of the imagery
    (if it is the other way round, try the other order).

COMMANDS
  .sat on | off | toggle        .sat opacity 40         .sat gray on | off
  .sat layer front | back       .sat maxzoom 16         .sat sharp -1..2
  .sat quality good | fast
  .sat cache          (show disk use)      .sat cache 6   (set the limit to 6 GB)
  .sat reload   (re-read SatView.ini)      .sat status    (state + last problem)

IF NOTHING APPEARS
  Type .sat status. If it says "imagery server answered HTTP 400" or similar, the service
  names in SatView.ini (WmtsLayer / TileMatrixSet) may need changing: send the message to
  whoever helps you with the plugin. "no connection" means a network/firewall problem.

PLEASE READ
  - The photos are not live: they are updated area by area every few years.
  - Use of the IGN service is subject to IGN's terms (https://cartes.gouv.fr/cgu). The credit
    is shown at the bottom left of the radar screen.
  - First version, not yet tested inside EuroScope: expect a round or two of fixes.

VERSION 0.2 (speed)
  The imagery is composed once into an off-screen picture and only copied to the screen on
  each EuroScope refresh. It is recomposed only when you pan/zoom, when tiles arrive, or when
  a setting changes. Changing the opacity does not even need a recompose.
  .sat status shows how long the last composing and the copy to the screen took.
  If the frame rate is still not good: lower MaxZoom (e.g. 16), set Sharpness=-1, or Quality=fast.
