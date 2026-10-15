# PlatformIO with the ESP-IDF framework: record crashes as the ESP-IDF
# component does, by wrapping the panic handler when the firmware is linked.
# (With the Arduino framework the core already wraps it and Pinscope uses the
# core's panic hook instead.)
Import("env")

if "espidf" in env.get("PIOFRAMEWORK", []):
    env.Append(CPPDEFINES=[("CONFIG_PINSCOPE_CRASH_CAPTURE", 1)])
    DefaultEnvironment().Append(LINKFLAGS=["-Wl,--wrap=esp_panic_handler"])
