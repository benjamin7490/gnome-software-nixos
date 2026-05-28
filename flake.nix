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

            # Add glib-testing to buildInputs as required by 51.alpha
            buildInputs = (oldAttrs.buildInputs or []) ++ [
              pkgs.glib-testing
            ];

            # Ensure the NixOS plugin is enabled in meson
            mesonFlags = (oldAttrs.mesonFlags or []) ++ [
              "-Dnixos=true"
            ];
          });
          default = self.packages.${system}.gnome-software;
        });

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
