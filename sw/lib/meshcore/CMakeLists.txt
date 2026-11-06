zephyr_include_directories(include)
target_sources(app PRIVATE
  src/advert.c
  src/ed25519.c
  src/mccrypto.c
  src/mesh.c
  src/packet.c
  src/sha512.c
)
