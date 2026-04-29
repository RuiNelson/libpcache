#!/usr/bin/env ruby

require "fileutils"

REPL = "./build/pcache_repl"

def run(*cmd)
  output = `#{cmd.join(" ")} 2>&1`
  status = $?.success?
  { :status => status, :output => output }
end

def run_repl_commands(db_path, data_path, *commands)
  cmd_file = "/tmp/repl_commands.txt"
  File.write(cmd_file, commands.join("\n"))
  result = run(REPL, "open", "-d", db_path, "-D", data_path, "-c", cmd_file)
  FileUtils.rm_f(cmd_file)
  result
end

ID_FILE   = "/tmp/test_id.bin"
PAGE_FILE = "/tmp/test_page.bin"
ID_FILE2  = "/tmp/test_id2.bin"
PAGE_FILE2 = "/tmp/test_page2.bin"
DB_PATH   = "/tmp/test.db"
DATA_PATH = "/tmp/test.data"

def setup
  FileUtils.rm_f(DB_PATH)
  FileUtils.rm_f(DATA_PATH)
  FileUtils.rm_f("#{DB_PATH}-shm")
  FileUtils.rm_f("#{DB_PATH}-wal")

  File.write(ID_FILE, "test-page-id-001")
  File.write(PAGE_FILE, "A" * 4096)
  File.write(ID_FILE2, "test-page-id-002")
  File.write(PAGE_FILE2, "B" * 4096)
end

def cleanup
  [ID_FILE, PAGE_FILE, ID_FILE2, PAGE_FILE2].each { |f| FileUtils.rm_f(f) }
  FileUtils.rm_f(DB_PATH)
  FileUtils.rm_f(DATA_PATH)
  FileUtils.rm_f("#{DB_PATH}-shm")
  FileUtils.rm_f("#{DB_PATH}-wal")
end

def query_db(sql)
  `sqlite3 #{DB_PATH} "#{sql}"`.strip
end

puts "=== pcache_repl tests ==="

setup
at_exit { cleanup }

# -- create command --
puts "\n[TEST] create volume"
result = run(REPL, "create", "-d", DB_PATH, "-D", DATA_PATH, "--page-size=4096", "--max-pages=100")
puts result[:output]
exit 1 unless result[:status]

schema = query_db("SELECT name FROM sqlite_master WHERE type='table'")
puts "Tables: #{schema}"
exit 1 unless schema.include?("metadata") && schema.include?("pages")

# -- help command --
puts "\n[TEST] help command"
result = run_repl_commands(DB_PATH, DATA_PATH, "help")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("help")

# -- put command --
puts "\n[TEST] put page"
result = run_repl_commands(DB_PATH, DATA_PATH, "put #{ID_FILE} #{PAGE_FILE}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("stored")

row_count = query_db("SELECT COUNT(*) FROM pages")
puts "Pages in DB: #{row_count}"
exit 1 unless row_count == "1"

# -- get command --
puts "\n[TEST] get page"
out_file = "/tmp/test_output.bin"
result = run_repl_commands(DB_PATH, DATA_PATH, "get #{ID_FILE} #{out_file}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("retrieved")

stored_content = File.read(PAGE_FILE, mode: "rb")
retrieved_content = File.read(out_file, mode: "rb")
exit 1 unless stored_content == retrieved_content
FileUtils.rm_f(out_file)

# -- check command (page exists) --
puts "\n[TEST] check page (exists)"
result = run_repl_commands(DB_PATH, DATA_PATH, "check #{ID_FILE}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("exists")

# -- check command (page does not exist) --
puts "\n[TEST] check page (not exists)"
result = run_repl_commands(DB_PATH, DATA_PATH, "check #{ID_FILE2}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("does not exist")

# -- pages command --
puts "\n[TEST] pages command"
result = run_repl_commands(DB_PATH, DATA_PATH, "pages")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("Used:")

# -- inspect command --
puts "\n[TEST] inspect command"
result = run_repl_commands(DB_PATH, DATA_PATH, "inspect")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("Page size:")

# -- set_max_pages command --
puts "\n[TEST] set_max_pages command"
result = run_repl_commands(DB_PATH, DATA_PATH, "set_max_pages 100")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("Max pages")

# -- defragment command --
puts "\n[TEST] defragment command"
result = run_repl_commands(DB_PATH, DATA_PATH, "defragment")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("complete")

# -- put second page --
puts "\n[TEST] put second page"
result = run_repl_commands(DB_PATH, DATA_PATH, "put #{ID_FILE2} #{PAGE_FILE2}")
puts result[:output]
exit 1 unless result[:status]

row_count = query_db("SELECT COUNT(*) FROM pages")
puts "Pages in DB: #{row_count}"
exit 1 unless row_count == "2"

# -- delete command --
puts "\n[TEST] delete page"
result = run_repl_commands(DB_PATH, DATA_PATH, "delete #{ID_FILE}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("deleted")

# -- check deleted page --
puts "\n[TEST] check deleted page"
result = run_repl_commands(DB_PATH, DATA_PATH, "check #{ID_FILE}")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("does not exist")

# -- delete with --wipe flag --
puts "\n[TEST] delete with --wipe flag"
result = run_repl_commands(DB_PATH, DATA_PATH, "delete #{ID_FILE2} --wipe")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("deleted")

# -- defragment with --shrink flag --
puts "\n[TEST] defragment with --shrink flag"
result = run_repl_commands(DB_PATH, DATA_PATH, "defragment --shrink")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("complete")

# -- set_max_pages with --durable flag --
puts "\n[TEST] set_max_pages with --durable flag"
result = run_repl_commands(DB_PATH, DATA_PATH, "set_max_pages 50 --durable")
puts result[:output]
exit 1 unless result[:status] && result[:output].include?("Max pages")

# -- error handling: get with missing file --
puts "\n[TEST] error handling: get with missing file"
result = run_repl_commands(DB_PATH, DATA_PATH, "get /nonexistent/file /tmp/out.bin")
puts result[:output]
exit 1 if result[:output].include?("OK")

# -- error handling: check with missing file --
puts "\n[TEST] error handling: check with missing file"
result = run_repl_commands(DB_PATH, DATA_PATH, "check /nonexistent/file")
puts result[:output]
exit 1 if result[:output].include?("OK")

puts "\n=== All tests passed ==="
exit 0