# invarlint terms

What the names on invarlint's pages mean. invarlint writes this file beside `findings.md` on every run; each rule name and column heading on `findings.md` and the as-found `INDEX.md` links here. Words of your own domain (a card or table your dictionary defines) link to your dictionary instead.

A **finding** is one row a rule produces at one commit. A **key** is what names it across commits: the rule, its subject and the function at the site. The **baseline** (`findings.baseline.json`) is the keys you have accepted; `check --baseline` names the keys that are new and the ones that no longer fire. A rule is either a **key**, which gates the build when a new one appears, or a **report**, an inventory that is never baselined.

## Reading findings.md

### subject
What the row is about: a column (`table.column`, or `table.*` for every column), a route, a card, a context, a function, a channel or a file template. A subject whose table your dictionary defines links to its entry.

### count
How many of the thing the rule counts: writers, statements, callers, tests. The rule's entry says which.

### sites
Where it happens: the function and its `file:line`, with a note after the semicolon (the callee, the context, the helper it went through). At most twelve are shown; `findings.json` has all.

### new
A key present at this commit that the baseline does not hold. It gates the build until it is fixed or the baseline is saved again with it (`check --save-baseline`).

### stale baseline
A baseline key that no longer fires: the thing was fixed or renamed. Saving the baseline again drops it.

### context
A bounded context: a part of the code that keeps one model and one vocabulary. A function's context is its card's `context`, else the context card naming its file, else its package.

### units
The packages (Go), source directories or files a context covers.

### funcs
The functions in the context.

### handlers
The functions in the context that serve a route.

### tables
The tables the context's functions write.

### calls into
The other contexts this one calls, with how many distinct functions it calls in each.

## Reading INDEX.md

`INDEX.md` lists the **as-found cards**: invarlint's photograph of the design the code has, one card per group of tables that share their writers. It is rewritten on every `distill` run; the cards you edit live one folder up.

### card
The as-found card's name, taken from its table with the most written columns. It links to your dictionary when an entry has that name.

### tables
The tables the card owns. A table joins a card when every function that writes it also writes the card's table (see Merged). A table links to the dictionary entry that claims it.

### direct
Functions that write one of the card's tables in their own body.

### attributed
Functions that write one of the card's tables through a helper: the write is counted at the caller, since the helper only does what it is told.

### ~ fields
Fields of the card the code could not fill and a person must judge, marked `~` in the card: `context` (no context card places it), `gate` (no route reaching it has a role gate), `collaborators.mode` (a call whose kind, command or query, sync or async, is a decision).

### related
Other cards that share direct writers with this one, as `Card shared/smaller`: the shared writers, of the smaller card's count. A high share suggests the two are one card.

### Merged
Tables that joined another table's card, as `from → into`: every function that writes the first also writes the second. A table with fewer than two direct writers never merges.

### Vocabulary
Seeded tables no function writes: lists of values the code reads (statuses, kinds). Each becomes a vocabulary card.

### External
Tables the code reads but no function writes and no seed fills: something outside the code fills them.

### Dead
Tables neither read nor written by the code.

### System
Tables the database keeps for itself (SQLite's `sqlite_` tables).

### Seeded but written by Go
Seeded tables some function also writes: an editor for the list, or a settings table.

### Ambiguous bindings
Bindings, as `Card binding`, that also match a function outside the card: a more specific binding settles which card it is.

## Keys: cards and their claims

A card's claims are what it says the code does: what it owns, who may reach it, how it is bound. These rules check them.

### card-unbound
A card whose `implemented_by` names no function the code has. Its other claims stay silent until it is bound.

### collaborator-undefined
A card's collaborator names no card.

### owns-unwritten
A card owns something no function writes.

### gate-unreached
A card declares gate roles, but no route reaches its functions, so the gate cannot be shown to hold.

### gate-violated
A route reaches a card's functions without a gate that accepts any of the card's roles.

### gate-unknown-role
A card names a role the roles table does not have.

### owns-violated
A write reaches a column a card owns by a path that does not go through the card's functions.

### vocabulary-written
A function writes a table declared as a vocabulary (a seeded list of values).

### extract-error
A file invarlint could not read; counts elsewhere are lower bounds until it is fixed.

## Keys: contexts and language

### root-bypassed
A call from another context lands on a function of a context that declares a root, but not on the root: the context's way in is skipped.

### context-straddled
A function bound by cards of two or more contexts with equally specific bindings. The map gives it to the first card by name; the cards must decide.

### foreign-writer
A function in one context writes a table another context owns.

### term-undefined
A word a context card says it knows has no entry in your dictionary.

### term-dangling
A dictionary entry claims a table the schema does not have.

## Keys: scenarios

A scenario card is one named path through a use case, held by a test.

### scenario-untested
A scenario with no test bound, or whose bindings match no test.

### scenario-step-untested
A scenario step (a route, a background job or a command) the code does not have, or that none of the scenario's tests drives.

### scenario-test-dangling
A test binding in a scenario that matches no test, while another binding does: a renamed or deleted test.

### scenario-term-undefined
A backticked phrase in a scenario's text with no entry in your dictionary.

## Keys: data and writes

### sync-where-async
A card declares a collaborator asynchronous, but its functions call the collaborator's functions directly.

### ungated-write-route
A route that reaches a database write with no role gate and no `access: public|internal` saying it is meant to be open.

### unowned-writer
A column written by two or more statements and owned by no card.

### episode-writer
The same as unowned-writer, but the writes are in a tool declared as an episode (a seeding or migration program), not the running service.

### multi-write-no-tx
A function that executes two or more writing queries outside a transaction: a failure between them leaves half the change.

### dropped-write-error
A function that discards, or only logs, the error from a call that writes.

### sql-clock
A query that reads the database's clock (`now()`, `CURRENT_TIMESTAMP`) on SQLite, whose time columns cannot carry a zone. Passing the time in as a parameter removes the key.

### full-replace-update
A handler that decodes a request body and updates two or more columns unconditionally, overwriting what the client did not send.

### copied-derivation
The same arithmetic, operand for operand, computed in two or more functions: one owner function removes the key.

### extern-drift
A C function declared in a `.c` file with a type that differs from its definition.

### extern-redeclared
A C function declared again in a `.c` file instead of a shared header.

### err-to-client
A function that writes an error's text into the HTTP response, showing the client internal detail.

## Reports

Reports are inventories: rows to read, never baselined, never gating the build.

### route-gates
Each route, its handler and the gates in front of it.

### context-coupling
Each pair of contexts joined by calls: how many distinct functions of the second the first calls.

### context-map
One row per context: its units, functions, handlers, tables written and the contexts it calls.

### context-size
The size of each context's functions: count, lines, median, the longest.

### stereotype-asfound
Each card's functions profiled by what they do (write, read, call other cards, compute, serve routes), and the kind of role that profile reads as, beside the one the card declares.

### mixed-dispatch
A function that runs in the background somewhere and is also called synchronously from a route's handler, where its work touches state. Shown at the first such function on the handler's path.

### multi-closer
A channel closed from two or more functions.

### channel-lifecycle
Each channel and every place it is made, sent on, received from and closed.

### spawn-sites
Each function that starts background work.

### hydration-leak
A package outside the data layer that imports the generated database code directly.

### writer-set
Every column with a writer, and its writers.

### file-flow
Each file path template, its writers and readers.

### path-flow
Each path template of a declared resource, its writers and readers.

### global-flow
Each global variable, its writers and readers.

### owns-exceeded
A card's function writes something the card does not own: a question for the card or the code.

### card-reach
Each card and the tests that reach its functions; a count of 0 is a card no test reaches.

### test-reach
Each test, what it reaches, and the keys it pins: keys whose function it runs, so a fix shows in the test.

### context-reach
Each context: the tests reaching it, its functions reached of the total, its keys pinned and unreached.

### scenario-reach
Each scenario with a test: the cards and contexts its tests reach, and the cards it names that they miss.
