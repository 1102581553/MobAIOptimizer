set_project("MobAIOptimizer")
set_version("1.0.0")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

add_requires("levilamina 1.9.5", {configs = {target_type = "server"}})
add_requires("levibuildscript")  -- 添加此依赖以使用 @levibuildscript 规则

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

if is_plat("windows") then
    add_cxflags(
        "/EHa", "/utf-8", "/W4",
        "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204"
    )
else
    add_cxflags("-O2", "-march=native", "-flto=auto")
end

add_defines("NOMINMAX", "UNICODE")
set_languages("c++20")
set_optimize("fast")
set_symbols("none")
set_exceptions("none")

add_includedirs("src")

target("MobAIOptimizer")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    set_kind("shared")
    add_headerfiles("src/*.h")
    add_files("src/*.cpp")
    add_packages("levilamina")
    add_syslinks("shlwapi", "advapi32")
    set_targetdir("bin")
    set_runtimes("MD")
