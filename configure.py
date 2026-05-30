#!/usr/bin/env python3
"""
configure.py — 插件配置生成器

工作流：
  1. python3 configure.py --gen-cmake    ← 扫描 lib/*/plugins/*/plugin.cmake，生成池配置
  2. vi pool_plugin_config.cmake         ← 编辑 ON/OFF
  3. python3 configure.py                ← 读 .cmake，生成 include/pool_plugin_config.h
  4. cmake -B build && cmake --build     ← 编译

plugin.cmake 格式：
  set(POOL_PLUGIN_{NAME}_SOURCES  相对路径/源文件.c)
  set(POOL_PLUGIN_{NAME}_INCLUDES 相对路径/include)
"""

import os, re, sys, argparse

HOOK_SIGNATURES = {
    "post_init":     {"params": "pool_cfg_t *cfg",                                "args": "cfg"},
    "post_alloc":    {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t start, uint32_t count, uint32_t handle",
                                                                                   "args": "cfg, owner, start, count, handle"},
    "pre_free":      {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle",
                                                                                   "args": "cfg, owner, handle"},
    "post_free":     {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle",
                                                                                   "args": "cfg, owner, handle"},
    "pre_lock":      {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle",
                                                                                   "args": "cfg, owner, handle"},
    "post_lock":     {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle, void *addr",
                                                                                   "args": "cfg, owner, handle, addr"},
    "pre_unlock":    {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle",
                                                                                   "args": "cfg, owner, handle"},
    "post_unlock":   {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle",
                                                                                   "args": "cfg, owner, handle"},
    "pre_resize":    {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle, uint32_t old_pages, uint32_t new_pages",
                                                                                   "args": "cfg, owner, handle, old_pages, new_pages"},
    "post_resize":   {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t handle, uint32_t old_pages, uint32_t new_pages",
                                                                                   "args": "cfg, owner, handle, old_pages, new_pages"},
    "defrag_move":   {"params": "pool_cfg_t *cfg, uint32_t src_page, uint32_t dst_page, uint32_t count, uint32_t handle_idx",
                                                                                   "args": "cfg, src_page, dst_page, count, handle_idx"},
    "pre_defrag":    {"params": "pool_cfg_t *cfg",                                "args": "cfg"},
    "post_defrag":   {"params": "pool_cfg_t *cfg",                                "args": "cfg"},
    "pre_free_all":  {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t forced",
                                                                                   "args": "cfg, owner, forced"},
    "post_free_all": {"params": "pool_cfg_t *cfg, pool_owner_t *owner, uint32_t forced",
                                                                                   "args": "cfg, owner, forced"},
}

HOOK_MACRO_PREFIX = "POOL_HOOK_"
PLUGIN_CMAKE_RE = re.compile(
    r'^set\(\s*POOL_PLUGIN_(\w+)_(SOURCES|INCLUDES)\s+(.+?)\s*\)', re.MULTILINE)


# ================================================================
# 模式 A：--gen-cmake — 扫描 plugin.cmake，生成 pool_plugin_config.cmake
# ================================================================

def scan_plugin_cmake_files(scan_dirs):
    """返回 [{name, prefix, sources, includes}]"""
    result = []
    for sd in scan_dirs:
        if not os.path.isdir(sd):
            continue
        for entry in sorted(os.listdir(sd)):
            lib = os.path.join(sd, entry)
            if not os.path.isdir(lib):
                continue
            pd = os.path.join(lib, "plugins")
            if not os.path.isdir(pd):
                continue
            for pname in sorted(os.listdir(pd)):
                cf = os.path.join(pd, pname, "plugin.cmake")
                if not os.path.isfile(cf):
                    continue
                with open(cf) as f:
                    txt = f.read()
                src = inc = ""
                for m in PLUGIN_CMAKE_RE.finditer(txt):
                    if m.group(1).lower() != pname.lower():
                        continue
                    if m.group(2) == "SOURCES":
                        src = m.group(3).strip()
                    else:
                        inc = m.group(3).strip()
                if not src:
                    continue
                prefix = f"{sd}/{entry}".replace("\\", "/")
                result.append({"name": pname, "prefix": prefix,
                               "sources": src, "includes": inc})
    return result


def generate_cmake(plugins, output, dry_run):
    lines = [
        "# Auto-generated by configure.py --gen-cmake — DO NOT EDIT",
        "# Edit options below (ON/OFF), then run configure.py to regenerate .h",
        "",
    ]
    for p in plugins:
        lines.append(f'option(POOL_USE_{p["name"].upper()} "Use {p["name"]} plugin" ON)')
    lines += ["",
              "set(POOL_PLUGIN_SOURCES \"\")",
              "set(POOL_PLUGIN_INCLUDES \"\")",
              "set(POOL_PLUGIN_NAMES \"\")",
              ""]
    for p in plugins:
        opt = f'POOL_USE_{p["name"].upper()}'
        src = " ".join(f'{p["prefix"]}/{s.strip()}' for s in p["sources"].split() if s.strip())
        inc = " ".join(f'{p["prefix"]}/{s.strip()}' for s in p["includes"].split() if s.strip()) if p["includes"] else ""
        plug_dir = f'{p["prefix"]}/plugins/{p["name"]}'
        lines += [
            f"if({opt})",
            f"    list(APPEND POOL_PLUGIN_SOURCES  {src})",
            f"    list(APPEND POOL_PLUGIN_INCLUDES {inc})",
            f"    list(APPEND POOL_PLUGIN_NAMES    {p['name']})",
            f"    set(POOL_PLUGIN_{p['name'].upper()}_DIR {plug_dir})",
            "endif()",
            "",
        ]
    content = "\n".join(lines) + "\n"
    if dry_run:
        print(content)
    else:
        with open(output, "w") as f:
            f.write(content)
        print(f"Generated {output}")
        print(f"Plugins: {', '.join(p['name'] for p in plugins) or '(none)'}")


# ================================================================
# 模式 B：生成 include/pool_plugin_config.h
# ================================================================

def read_cmake_plugin_dirs(cmake_path):
    """从 pool_plugin_config.cmake 读取启用的插件名和对应的 plugin_dir"""
    if not os.path.isfile(cmake_path):
        return {}
    with open(cmake_path) as f:
        txt = f.read()
    dirs = {}
    for m in re.finditer(r"set\(POOL_PLUGIN_(\w+)_DIR\s+(.+?)\s*\)", txt):
        name = m.group(1).lower()
        plug_dir = m.group(2).strip()
        # 确认这个名字在 POOL_PLUGIN_NAMES 中
        if re.search(rf'list\s*\(\s*APPEND\s+POOL_PLUGIN_NAMES\s+{re.escape(name)}\b', txt):
            dirs[name] = plug_dir
    return dirs


def scan_plugins(plugin_dir):
    """扫描 {plugin_dir}/*/plugin.h，返回 {name: [hooks]}"""
    plugins = {}
    if not os.path.isdir(plugin_dir):
        return plugins
    for entry in sorted(os.listdir(plugin_dir)):
        pp = os.path.join(plugin_dir, entry)
        if not os.path.isdir(pp):
            continue
        header = os.path.join(pp, "plugin.h")
        if not os.path.isfile(header):
            continue
        with open(header) as f:
            content = f.read()
        hooks = []
        for hook_name in HOOK_SIGNATURES:
            if re.search(rf'\b{re.escape(entry)}_{re.escape(hook_name)}\s*\(', content):
                hooks.append(hook_name)
        if hooks:
            plugins[entry] = hooks
    return plugins


def generate_h(plugins, output, dry_run):
    """生成的 .h 中的 include 路径相对于 include/ 目录"""

    # 收集并去重 plugin_dir 的父目录（多个插件可能在同一目录）
    # 每个 entry 的格式是 "lib/pool_log/plugins/logging"
    # 所以包含路径是 "../{parent_dir}/{name}/plugin.h"
    # 其中 parent_dir = os.path.dirname(plugin_dir_of_entry) = "lib/pool_log/plugins"
    # 但 generate_config 期望一个统一的 plugin_dir 参数...
    # 所以需要修改 generate_config 或者单独处理每个插件

    lines = [
        "/* Auto-generated by configure.py — DO NOT EDIT */",
        "/*",
        f" * Plugins found: {', '.join(sorted(plugins)) if plugins else '(none)'}",
        " */",
        "",
        "#ifndef POOL_PLUGIN_CONFIG_H",
        "#define POOL_PLUGIN_CONFIG_H",
        "",
    ]

    # 为每个插件生成单独的 #include
    for name in sorted(plugins):
        plug_dir = plugin_dirs.get(name, "")
        if plug_dir:
            # plug_dir 类似 "lib/pool_log/plugins/logging"
            # 从 include/ 出发 ../lib/pool_log/plugins/logging/plugin.h
            lines.append(f'#include "../{plug_dir}/plugin.h"')

    if plugins:
        lines.append("")

    # Hook 宏定义
    for hook_name in HOOK_SIGNATURES:
        macro = f"{HOOK_MACRO_PREFIX}{hook_name.upper()}"
        sig = HOOK_SIGNATURES[hook_name]
        active = [n for n in sorted(plugins) if hook_name in plugins[n]]
        if not active:
            continue
        lines += [
            f"/* {hook_name} — implemented by: {', '.join(active)} */",
            f"#undef {macro}",
            f"#define {macro}({sig['args']}) \\",
        ]
        calls = "; \\\n".join(f"    {n}_{hook_name}(&{n}_ctx, {sig['args']})" for n in active)
        lines.append(f"{calls};")
        lines.append("")

    lines.append("#endif /* POOL_PLUGIN_CONFIG_H */")
    content = "\n".join(lines) + "\n"

    if dry_run:
        print(content)
    else:
        os.makedirs(os.path.dirname(output), exist_ok=True)
        with open(output, "w") as f:
            f.write(content)
        print(f"Generated {output}")


# ================================================================
# 主流程
# ================================================================

def main():
    parser = argparse.ArgumentParser(description="Pool plugin configuration generator")
    parser.add_argument("--gen-cmake", action="store_true",
                        help="Generate pool_plugin_config.cmake from plugin.cmake files")
    parser.add_argument("--scan-dir", action="append", default=[],
                        help="Directory to scan for plugins (can repeat)")
    parser.add_argument("--cmake-output", default="pool_plugin_config.cmake",
                        help="Output path for .cmake")
    parser.add_argument("--output", default="include/pool_plugin_config.h",
                        help="Output header path")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print output instead of writing")
    args = parser.parse_args()

    if args.gen_cmake:
        # 模式 A
        scan_dirs = args.scan_dir or ["lib"]
        plugins = scan_plugin_cmake_files(scan_dirs)
        generate_cmake(plugins, args.cmake_output, args.dry_run)
    else:
        # 模式 B
        global plugin_dirs
        plugin_dirs = read_cmake_plugin_dirs("pool_plugin_config.cmake")
        all_plugins = {}
        for name, plug_dir in plugin_dirs.items():
            # scan_plugins 扫描 plug_dir 的父目录下的子目录
            parent = os.path.dirname(plug_dir) or "."
            scanned = scan_plugins(parent)
            if name in scanned:
                all_plugins[name] = scanned[name]
        generate_h(all_plugins, args.output, args.dry_run)


plugin_dirs = {}
if __name__ == "__main__":
    main()
