Import("env")

MAX_FACTORY_BYTES = 500 * 1024


def check_factory_size(source, target, env):
    path = str(target[0])
    size = __import__("os").path.getsize(path)
    print(f"Recovery image: {size} / {MAX_FACTORY_BYTES} bytes")
    if size > MAX_FACTORY_BYTES:
        raise RuntimeError(
            f"factory firmware is {size} bytes; recovery cap is {MAX_FACTORY_BYTES}"
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_factory_size)
