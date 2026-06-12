import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
binp = "/tmp/stress_test_ci"

c = subprocess.run(["c++", "-std=c++17", "-I", ROOT + "/src", "-O0", "-o", binp,
                    ROOT + "/tests/stress_test.cpp", ROOT + "/src/fs.cpp"],
                   capture_output=True, text=True)
if c.returncode != 0:
    print(c.stderr)
    sys.exit("compile failed")

r = subprocess.run([binp, "50"], cwd=ROOT, capture_output=True, text=True)
print(r.stdout[-600:])

mism = None
for line in r.stdout.splitlines():
    if "mismatches:" in line:
        mism = int(line.split(":")[1].strip())

ok = (r.returncode == 0) and (mism == 0)
print("test_stress: PASS" if ok else "test_stress: FAIL")
sys.exit(0 if ok else 1)
