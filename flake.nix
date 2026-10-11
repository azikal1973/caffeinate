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
        # statisch gelinkt (musl): laeuft auch ausserhalb von NixOS
        static = pkgs.pkgsStatic.callPackage ./package.nix { };
      });

      overlays.default = final: _prev: {
        caffeinate = final.callPackage ./package.nix { };
      };

      # make test samt D-Bus-Test: nix develop -c make test
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [
            pkgs.dbus
            pkgs.util-linux
            (pkgs.python3.withPackages (p: [ p.pygobject3 ]))
            # Xvfb fuer tests/x11.sh
            (pkgs.xorg-server or pkgs.xorg.xorgserver)
          ];
          # PyGObject findet die Typelibs von GLib/Gio hierueber
          GI_TYPELIB_PATH = lib.makeSearchPath "lib/girepository-1.0" [
            pkgs.glib.out
            pkgs.gobject-introspection
          ];
        };
      });
    };
}
