# ps4-hen-plugins

Plugin system for Updated PS4 Homebrew Enabler [ps4-hen](https://github.com/Scene-Collective/ps4-hen).

# Plugins

### `plugin_mono`
  - Patches ShellUI to allow more features.
  - Features:
  - Package Installer:
    - Show path to package files
    - **New!** Install Packages from Hard Disk (`/data/pkg`)
    - Install Package from USB Storage
  - Plugins Loader
    - Plugins will be loaded on game (BigApp) launch.
    - Manual adjust by config file in `/data/hen/plugins.ini`
    - Supports `all` for all apps or by individual TitleID (Not supported yet in UI).
  - Plugins Manager
    - Manages plugins installed under `/data/hen/plugins`.
    - Currently only supports enabling from `all` section for now.
  - Adjust HEN Payload settings.
  - Based from [Orbis-Toolbox](https://github.com/OSM-Made/Orbis-Toolbox) and research by [OSM-Made](https://github.com/OSM-Made)
### `plugin_example`
  - Demonstrate usage of CXX in module.
  - Based from OpenOrbis [`library_example`](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/63c0be5ffff09fbaebebc6b9a738d150e2da0205/samples/library_example/library_example/lib.cpp)
### `plugin_server`
  - Starts klog on port 3232 <!-- (assuming process has access to `/dev/klog`, i.e `ScePartyDaemonMain`) -->
  - Based on [klogsrv](https://github.com/ps5-payload-dev/klogsrv)
  - Starts FTP server on port 2121.
  - Based on [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv)
    - **Note: No SELF decryption yet.**
    - klogsrv and ftpsrv is licensed under GPL3 but this repository and it's code is licensed under MIT. The notice for mentioned projects is reproduced below.
    - <details> <summary> Notice (Click to view) </summary>
      
      ```
      Copyright (C) 2023 John Törnblom
      
      This program is free software; you can redistribute it and/or modify it
      under the terms of the GNU General Public License as published by the
      Free Software Foundation; either version 3, or (at your option) any
      later version.
      
      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License for more details.
      
      You should have received a copy of the GNU General Public License
      along with this program; see the file COPYING. If not, see
      <http://www.gnu.org/licenses/>.
      ```
      
      </details>

# Credits

- [GoldHEN Plugin SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) for minimal crt.
- [OpenOrbis Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) for toolchain.
- [klogsrv](https://github.com/ps5-payload-dev/klogsrv) for klog server.
- [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) for FTP server.
  - [libjbc](https://github.com/sleirsgoevy/ps4-libjbc) for process jailbreak and sandbox escape.
- [Orbis-Toolbox](https://github.com/OSM-Made/Orbis-Toolbox) for Mono patching references.
- [OSM-Made](https://github.com/OSM-Made) for Mono patching references.
