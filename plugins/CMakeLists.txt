# Listery 插件 / Plugins
#
# 插件是 MODULE 库，不链接 Qt：只用 C 标准库 + 宿主回调，
# 因此同一份源码可以在 Qt 5.7(XP)、Qt 5.15、Qt 6 的 Listery 上加载。
add_subdirectory(translator)
