// Copyright (c) 2026 Ekorau LLC

package load

import (
	"strings"
	"testing"
	"time"
)

// vm is a host VM that prints script and then stays up until it is stopped.
func vm(script string) Host {
	return Host{Argv: []string{"sh", "-c", script + "; sleep 30", "vm"}, Timeout: 5 * time.Second}
}

func TestHostProvesTheProgramItRan(t *testing.T) {
	sha := fixtureSHA(t)
	t0 := time.Now()
	res, err := vm("echo 'oevm: build-id host'; echo \"oevm: program sha256 " + sha + "\"").Load(fixture)
	if err != nil {
		t.Fatal(err)
	}
	if res.SHA256 != sha || res.BuildID != "host" || res.Offset != 0 || res.Image != fixture {
		t.Fatalf("%+v", res)
	}
	if time.Since(t0) > 3*time.Second {
		t.Fatal("the VM was not stopped at the complete boot")
	}
}

func TestHostReportsTheLoadersRefusal(t *testing.T) {
	_, err := vm("echo 'oevm: build-id host'; echo 'oimg_load_blob: registry_version 7 != firmware 8' >&2").Load(fixture)
	if err == nil || !strings.Contains(err.Error(), "registry_version 7 != firmware 8") {
		t.Fatalf("err %v", err)
	}
}

func TestHostFailsWhenTheVMRunsAnotherProgram(t *testing.T) {
	_, err := vm("echo 'oevm: build-id host'; echo 'oevm: program sha256 " + strings.Repeat("f", 64) + "'").Load(fixture)
	if err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Fatalf("err %v", err)
	}
}

func TestHostFailsWhenTheVMEndsEarly(t *testing.T) {
	h := Host{Argv: []string{"sh", "-c", "echo 'oevm: build-id host'", "vm"}, Timeout: 5 * time.Second}
	if _, err := h.Load(fixture); err == nil || !strings.Contains(err.Error(), "ended before") {
		t.Fatalf("err %v", err)
	}
}

func TestHostRefusesANonImage(t *testing.T) {
	if _, err := vm("exit 1").Load("host_test.go"); err == nil || !strings.Contains(err.Error(), "not an .oimg") {
		t.Fatalf("err %v", err)
	}
}
