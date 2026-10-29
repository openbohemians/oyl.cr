# Builds Oyl, the C library vendored in ext/oyl, into ext/oyl/build:
# liboyl.a with cc and ar, or on Windows oyl.lib with MSVC's cl and lib.
# The shard's postinstall runs it:
#
#   crystal run ext/build.cr
#
# On Unix, CC, AR and CFLAGS choose the tools and flags, as in Makefile.
# On Windows, cl is found on PATH, or else through vswhere and the
# Visual Studio command prompt's environment.

OYL   = Path[__DIR__] / "oyl"
BUILD = OYL / "build"

def run(command : String, args : Array(String)) : Nil
  puts "#{command} #{args.join(' ')}"
  status = Process.run(command, args, output: STDOUT, error: STDERR)
  abort "#{command} failed (#{status.exit_code})" unless status.success?
end

sources = Dir.children(OYL / "src").select(&.ends_with?(".c")).sort.map { |name| OYL / "src" / name }
Dir.mkdir_p(BUILD)

{% if flag?(:win32) %}
  # Without cl on PATH, take the environment Visual Studio's command
  # prompt sets up (PATH, INCLUDE, LIB).
  unless Process.find_executable("cl")
    vswhere = Path[ENV["ProgramFiles(x86)"]? || "C:/Program Files (x86)"] / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    abort "cl isn't on PATH and Visual Studio wasn't found: run this from a Visual Studio command prompt" unless File.exists?(vswhere)
    install = String.build do |io|
      Process.run(vswhere.to_s, ["-latest", "-products", "*", "-property", "installationPath"], output: io)
    end.strip
    vcvars = Path[install] / "VC" / "Auxiliary" / "Build" / {{ flag?(:aarch64) ? "vcvarsarm64.bat" : "vcvars64.bat" }}
    abort "MSVC wasn't found: run this from a Visual Studio command prompt" if install.empty? || !File.exists?(vcvars)
    # a batch file, so that cmd needn't parse nested quotes
    bat = File.tempfile("oyl-vcvars", ".bat") { |f| f.print %(@call "#{vcvars}" >nul\r\n@set\r\n) }
    env = String.build { |io| Process.run("cmd.exe", ["/c", bat.path], output: io) }
    bat.delete
    env.each_line do |line|
      name, eq, value = line.chomp.partition('=')
      ENV[name] = value unless eq.empty? || name.empty?
    end
    abort "cl wasn't found after #{vcvars}" unless Process.find_executable("cl")
  end
  objects = sources.map do |source|
    object = BUILD / "#{source.stem}.obj"
    run "cl", ["/nologo", "/std:c11", "/O2", "/MD", "/I#{OYL / "include"}", "/c", source.to_s, "/Fo#{object}"]
    object.to_s
  end
  run "lib", ["/nologo", "/OUT:#{BUILD / "oyl.lib"}"] + objects
{% else %}
  cc = ENV["CC"]? || "cc"
  cflags = (ENV["CFLAGS"]? || "-O2").split
  objects = sources.map do |source|
    object = BUILD / "#{source.stem}.o"
    run cc, ["-std=c11"] + cflags + ["-I#{OYL / "include"}", "-fvisibility=hidden", "-c", source.to_s, "-o", object.to_s]
    object.to_s
  end
  archive = BUILD / "liboyl.a"
  File.delete?(archive)
  run ENV["AR"]? || "ar", ["rcs", archive.to_s] + objects
{% end %}
