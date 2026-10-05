module github.com/davidg238/rete/custos

go 1.26.1

require (
	github.com/davidg238/rete/protocol v0.0.0
	github.com/mattn/go-sqlite3 v1.14.40
	gopkg.in/yaml.v3 v3.0.1
)

replace github.com/davidg238/rete/protocol => ../protocol
