{
  description = "caffeinate fuer Linux: Nachbau von macOS caffeinate(8)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      systems = lib.filter (lib.hasSuffix "-linux") lib.systems.flakeExposed;
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./package.nix { };
        # statically linked (musl): also runs outside NixOS
        static = pkgs.pkgsStatic.callPackage ./package.nix { };
      });

      overlays.default = final: _prev: {
        caffeinate = final.callPackage ./package.nix { };
      };

      # everything make test needs, D-Bus and X11 tests included: nix develop -c make test
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [
            pkgs.dbus
            pkgs.util-linux
            (pkgs.python3.withPackages (p: [ p.pygobject3 ]))
            # Xvfb for tests/x11.sh
            (pkgs.xorg-server or pkgs.xorg.xorgserver)
          ];
          # this is how PyGObject finds the typelibs of GLib/Gio
          GI_TYPELIB_PATH = lib.makeSearchPath "lib/girepository-1.0" [
            pkgs.glib.out
            pkgs.gobject-introspection
          ];
        };
      });
    };
}
