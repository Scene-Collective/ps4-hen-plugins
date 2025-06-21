# ps4-hen-plugins

Plugin system for Updated PS4 Homebrew Enabler [ps4-hen-vtx](https://github.com/EchoStretch/ps4-hen-vtx).

# Plugins

- `plugin_example`
  - Demonstrate usage of CXX in module. Based from OpenOrbis [`library_example`](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/63c0be5ffff09fbaebebc6b9a738d150e2da0205/samples/library_example/library_example/lib.cpp)
- `plugin_server`
  - Starts klog on port 3232 (assuming process has access to `/dev/klog`, i.e `ScePartyDaemonMain`)
  - Based on [klogsrv](https://github.com/ps5-payload-dev/klogsrv)

# Credits

- [GoldHEN Plugin SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK) for minimal crt.
- [OpenOrbis Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) for toolchain.
- [klogsrv](https://github.com/ps5-payload-dev/klogsrv) for klog server.
