{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "env";
  buildInputs = with pkgs; [
    rebar3
    cmake
    ninja
    pkg-config
    swig
    pcre
    bison
    flex
    openssl
  ];
}
