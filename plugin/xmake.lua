includes("../lib/commonlibsse-ng")

set_project("CouriersLedger")
set_version("1.1.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release", "mode.releasedbg")

target("CouriersLedger")
    add_rules("commonlibsse-ng.plugin", {
        name = "CouriersLedger",
        author = "dqann and Claude",
        description = "The Courier's Ledger: everyone you have met, where they are, what they sell"
    })
    add_files("src/**.cpp")
    add_includedirs("src", "include")
    set_pcxxheader("src/pch.h")
