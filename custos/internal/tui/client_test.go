// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestClientDecodesAndRefuses(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /debug/state", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("after") != "4" {
			t.Errorf("after = %q", r.URL.Query().Get("after"))
		}
		w.Write([]byte(`{"Seq":5,"Run":"stopped","Stop":{"Reason":"break","File":"a.nsl","Line":16}}`))
	})
	mux.HandleFunc("POST /debug/do", func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "custos: a command is already running", http.StatusConflict)
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()
	c := NewClient(srv.URL + "/")
	st, err := c.State(4)
	if err != nil || st.Seq != 5 || st.Stop.Line != 16 {
		t.Fatalf("State: %+v %v", st, err)
	}
	_, err = c.Do("step")
	var se *StatusError
	if !errors.As(err, &se) || se.Code != 409 {
		t.Fatalf("Do on 409: %v", err)
	}
	if _, err := c.Last(); !errors.As(err, &se) || se.Code != 404 {
		t.Fatalf("Last with no route: %v", err)
	}
}
