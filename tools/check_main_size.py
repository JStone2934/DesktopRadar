Import("env")

MAX_MAIN_BYTES = 1_650_000


def check_main_size(source, target, env):
    path = str(target[0])
    size = __import__("os").path.getsize(path)
    print(f"OTA main image: {size} / {MAX_MAIN_BYTES} bytes")
    if size > MAX_MAIN_BYTES:
        raise RuntimeError(
            f"main firmware is {size} bytes; OTA release cap is {MAX_MAIN_BYTES}"
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_main_size)
