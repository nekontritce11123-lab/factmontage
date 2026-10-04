"""Compare the production frei0r renderer with its scalar reference."""
import ctypes as C
import sys

width, height = 96, 54
size = width * height * 4
Buffer = C.c_uint8 * size


def plugin(path):
    lib = C.CDLL(path)
    lib.f0r_construct.argtypes = [C.c_uint, C.c_uint]
    lib.f0r_construct.restype = C.c_void_p
    lib.f0r_set_param_value.argtypes = [C.c_void_p, C.c_void_p, C.c_int]
    lib.f0r_update2.argtypes = [C.c_void_p, C.c_double, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
    lib.f0r_destruct.argtypes = [C.c_void_p]
    assert lib.f0r_init()
    return lib, lib.f0r_construct(width, height)


def main():
    plugins = [plugin(path) for path in sys.argv[1:]]
    assert len(plugins) == 2
    first, second = Buffer(), Buffer()
    results = [Buffer(), Buffer()]
    worst = 0
    try:
        for alpha in (127, 255):
            for i in range(0, size, 4):
                first[i:i + 4] = ((i * 7) % 251, (i * 3) % 253, 40, alpha)
                second[i:i + 4] = (30, (i * 11) % 251, (i * 5) % 253, alpha)
            for style in range(15):
                code = style / 11 if style < 12 else (2 * (style - 12) + 1) / 22
                for direction in range(4):
                    for quality in (0, 1):
                        for progress in (0, .25, .5, .75, 1):
                            for (lib, instance), output in zip(plugins, results):
                                for index, value in ((0, progress), (1, code), (3, direction / 3), (6, quality)):
                                    lib.f0r_set_param_value(instance, C.byref(C.c_double(value)), index)
                                lib.f0r_update2(instance, 0, first, second, None, output)
                            delta = max(abs(a - b) for a, b in zip(*results))
                            assert delta <= 2, (style, direction, quality, progress, alpha, delta)
                            worst = max(worst, delta)
    finally:
        for lib, instance in plugins:
            lib.f0r_destruct(instance)
            lib.f0r_deinit()
    print(f'PASS all 15 styles, 4 directions, alpha and quality: scalar/SSE2 maximum difference {worst}/255')


if __name__ == '__main__':
    main()
