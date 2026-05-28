{
  description = "GNOME Software adapted for NixOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          gnome-software = pkgs.gnome-software.overrideAttrs (oldAttrs: {
            pname = "gnome-software-nixos";
            version = "51.alpha-nixos";
            src = self;

            buildInputs = (oldAttrs.buildInputs or []) ++ [
              pkgs.glib-testing
            ];

            mesonFlags = (oldAttrs.mesonFlags or []) ++ [
              "-Dnixos=true"
            ];
          });
          default = self.packages.${system}.gnome-software;
        });

      # NixOS module — remplace gnome-software par la version NixOS
      nixosModules.default = { pkgs, lib, config, ... }: {
        options.programs.gnome-software-nixos.enable =
          lib.mkEnableOption "GNOME Software avec support NixOS natif";

        config = lib.mkIf config.programs.gnome-software-nixos.enable {
          nixpkgs.overlays = [
            (final: prev: {
              gnome-software = self.packages.${prev.stdenv.hostPlatform.system}.gnome-software;
            })
          ];

          environment.systemPackages = [ pkgs.gnome-software ];

          # Autorise pkexec nixos-rebuild via polkit pour les utilisateurs wheel
          security.polkit.extraConfig = ''
            polkit.addRule(function(action, subject) {
              if (action.id == "org.freedesktop.systemd1.manage-units" &&
                  subject.isInGroup("wheel")) {
                return polkit.Result.YES;
              }
            });
          '';
        };
      };

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.gnome-software ];
          };
        });
    };
}
