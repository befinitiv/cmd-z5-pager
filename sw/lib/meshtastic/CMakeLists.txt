zephyr_include_directories(include)
target_sources(app PRIVATE
  src/aes.c
  src/channel.c
  src/mtcrypto.c
  src/nodedb.c
  src/packet.c
  src/router.c
  src/sha256.c
  src/x25519.c
)
