# Generic `UiNode` and Surface Architecture

Status: accepted architectural direction; implementation pending  
Decision date: 2026-07-11  
Recorded: 2026-07-13  
Scope: core object model, runtime/driver boundary, capture, input, and events

## Provenance

This document records the architecture decision made before the external
reference-corpus study. It preserves that decision as its own design baseline:
browser and tab concepts are removed from the core in favor of generic UI nodes,
orthogonal presentation surfaces, object-scoped capabilities, and generic routing.

The later reference study did not originate this model. It validated the direction
and refined the lifetime and snapshot vocabulary. Those refinements are identified
separately near the end so that the original decision is not rewritten
retroactively.

## Decision

Grab will model every automatable application entity as a recursive `UiNode`.
Application, document, tab, panel, dialog, popup, child window, canvas region, and
draggable region are roles or properties of nodes, not separate core classes or
backend domains.

A `Surface` is an orthogonal presentation object describing where and how a node
is rendered, captured, hit-tested, and mapped into an input coordinate space.
`Surface` is **not** a replacement name for `Tab`: a tab is normally a child
`UiNode` and may have no native surface of its own.

Platform and integration code ends at a runtime/driver boundary. Drivers
contribute node facts, surface facts, relations, facets, action routes, and changes
to the same model. They do not introduce public browser, accessibility, X11, or
platform-specific object hierarchies.

## Why this replaces the current shape

The current code separates concerns into independently opened and resolved
domains:

- [`Screen`](include/grab/screen.hpp) opens capture state and accepts raw
  native window identifiers;
- [`Input`](include/grab/input.hpp) separately opens a seat, keymap, and
  window locator, then exposes window-specific interaction helpers;
- the provider registry resolves one global capability at a time;
- event sources maintain another representation of applications and windows;
- [`Event`](include/grab/event.hpp) makes `Browser`,
  `BrowserTabSwitched`, and `BrowserTab` canonical core concepts;
- the browser classifier infers those concepts from application names and window
  titles.

This creates pairwise glue. Capture needs to rediscover the target known to input;
input needs the geometry known to the window tracker; events need to correlate
their application strings with both; accessibility and browser integrations add
more identities that every existing subsystem must learn about.

It also permits an invalid composition: the best-ranked capture provider and the
best-ranked pointer provider may disagree about the target, coordinate space,
permission scope, coordinate mapping, or even which desktop they are attached to.
Adding Wayland, Windows, macOS, and richer semantic integrations would multiply
that problem.

The core needs one shared target and presentation model. Source-specific details
belong in drivers.

## Core vocabulary

The initial logical record is deliberately small:

```cpp
struct UiNode {
    NodeId        id;
    RoleId        role;          // application, document, dialog, panel, region...
    StateSet      states;        // active, focused, visible, selected...
    PropertySet   properties;    // typed title, URI, application identity...
    CapabilitySet capabilities;  // object-scoped behavior currently available
};
```

The names describe these concepts:

| Concept | Meaning |
|---|---|
| `UiNode` | A generic logical or semantic application entity. |
| `RoleId` | Descriptive vocabulary such as `application`, `document`, `tab`, `dialog`, `panel`, or `region`; it never selects a backend or C++ subclass. |
| state/property | Typed facts about a node. Source and confidence remain attached to contributed facts. |
| capability/facet | Behavior available on this particular node, such as invoke, text, selection, value, focus, drag source, or drop target. |
| relation | A typed edge between nodes or between a node and its presentation. |
| `Surface` | A physical presentation/capture/hit-test boundary with a coordinate space and runtime authority. |
| node selector | A query over roles, states, properties, capabilities, and relations; it contains no native handle. |
| `NodeRef` | A resolved identity used to refer to a `UiNode`; its detailed lifetime and staleness contract was left open at this stage. |
| route | A concrete semantic, capture, pointer, or keyboard operation supplied by a runtime. |

Adding a role such as `tab`, `sheet`, or `draggable-region` must not require a new
provider interface, public API class, event category, or source folder.

## Logical hierarchy and physical presentation are different graphs

A single parent pointer cannot represent both application structure and where
pixels/input live. Grab therefore keeps two linked views:

```text
Logical UI graph                         Presentation graph
-------------------------------          -------------------------------
UiNode: application                      Surface: native main window
├── UiNode: active document ─────────┐   ├── content region / viewport
├── UiNode: settings panel ────────┐ │   ├── toolbar region
├── UiNode: popup dialog ────────┐ │ │   └── overlay region
└── UiNode: draggable area ────┐ │ │ │
                               ▼ ▼ ▼ ▼
                         presented_on / occupies

UiNode: popup dialog ── presented_on ──> Surface: transient native window
```

Logical relations describe meaning and ownership. Presentation relations describe
rendering, geometry, and routes. They may disagree without either being wrong:

- a child tab can share its application's native surface;
- a popup can be logically owned by one node but occupy another native surface;
- an embedded renderer can be a logical child while living in another process;
- one node can be mirrored on more than one surface;
- a virtualized or hidden node can exist without a current surface;
- one surface can present many nodes and overlays.

The minimum relation vocabulary is:

- `contains`: logical hierarchy;
- `owns`: lifecycle ownership, which need not equal containment;
- `presented_on`: links a node to a surface or presentation region;
- `occupies`: gives bounds in a named presentation coordinate space;
- `overlays`: popup, menu, dialog, or transient overlay relationship;
- `active_child`: current document, panel, page, sheet, or tab;
- `focus_within`: keyboard focus scope and routing;
- `embeds`: one application or tree embedding another;
- `controls`: semantic control relationship.

Additional typed relations can be added without changing the node type. A generic
stringly parent/child tree is not sufficient, but neither is a subclass for every
relation.

## Browser and tab are ordinary cases

Browser protocol support is valuable, but it is a source of richer facts and
routes rather than a public ontology.

| User-visible thing | Core representation |
|---|---|
| Browser | `UiNode(role=application)` presented on one or more native surfaces. |
| Browser tab/page | Child `UiNode` with a document/tab role, properties such as title and URI, and selected/active state; the parent points to the current child through `active_child`. It is normally presented inside the parent surface. |
| Browser popup | Child or owned `UiNode`; it may share a surface or be presented on a transient native surface. |
| Settings panel | Child panel/document node presented within the application surface. |
| IDE editor tab or Electron webview | The same shape as a browser document; no IDE/Electron-specific core type. |
| Terminal pane or spreadsheet sheet | Child node with its own role, properties, and facets, commonly sharing the parent surface. |
| Draggable area | Region node with bounds and drag-source/drop-target facets. |
| Canvas-only control | Region node located by presentation or visual evidence and acted on through a physical route. |

There is no `BrowserWindow`, `BrowserTab`, or browser-specific capture/input/event
API. Names such as `tab` remain useful role vocabulary for users and selectors;
they do not create architectural branches.

Browser-only facts such as URL, origin, DOM attributes, network state, or a
JavaScript realm use typed, namespaced properties and facets contributed by a
WebExtension, CDP, or WebDriver BiDi driver. Another driver can still address the
same node through accessibility, pixels, native window metadata, or physical
input.

## Drivers contribute evidence and routes, not domain objects

A runtime integration attaches to one coherent native authority, permission and
lifetime scope, event-loop domain, and coordinate mapping. It may contribute any
combination of:

- nodes, roles, states, properties, and typed relations;
- surfaces, bounds, coordinate spaces, and transforms;
- semantic action facets;
- capture, pointer, keyboard, and observation routes;
- graph updates and lifecycle changes;
- aliases and evidence relating its native identities to existing nodes.

Examples:

- X11 contributes application/window nodes, native surfaces, bounds, focus,
  capture, pointer, keyboard, and events from one X11 authority;
- a WebExtension or browser protocol contributes child document nodes,
  `active_child` changes, titles, URIs, navigation state, semantic actions, and
  protocol aliases;
- AT-SPI contributes documents, dialogs, panels, controls, roles, states, bounds,
  relations, and semantic actions to the same graph;
- Wayland/portal integrations contribute authorized surfaces, streams, input
  sessions, and explicit coordinate mappings under one lease-aware plan;
- evdev contributes device/seat observations and origin information;
- UIA and AX later contribute the same generic facts and routes on Windows and
  macOS.

When two drivers describe the same logical object, a correlator records and
evaluates identity evidence. It may fuse identities only when the evidence policy
permits it; otherwise the nodes remain related or ambiguous. A matching title,
PID, role, or rectangle alone is never canonical identity.

A driver can be partial. Coherence does not mean one universal backend class must
implement everything. It means the session composes partial runtimes through
explicit identity and coordinate bridges, rather than independently choosing
unrelated providers because their capability strings happen to match.

## Generic cross-capability routing

Operations start with a node or node selector and follow generic relations. They
do not dispatch on application type.

1. Capture follows `presented_on` to the nearest compatible capturable surface,
   maps through a trusted coordinate transform, and crops to `occupies` when
   appropriate.
2. Pointer input follows `occupies -> presented_on`, maps the target point into
   the owning seat's coordinate space, and selects a route sharing that mapping
   authority.
3. Keyboard input follows `focus_within` to the appropriate focus scope and
   keyboard route.
4. Events name the affected node identity and relation/property change; subtree
   subscriptions work for any application hierarchy.
5. Semantic actions such as `invoke`, `select`, `set_value`, `drag`, or `close`
   prefer an exact object facet. Synthesized pointer/keyboard input is an explicit
   policy fallback, not a silent substitution.

Once a newly discovered node has presentation and route relationships, it gains
capture, input, and observation behavior without application-specific glue.

Illustrative target-bound interaction API from the original proposal, not a
frozen ABI:

```cpp
auto app = session->applications().find({.app_id = "org.example.Editor"});

auto child = app->descendants().find({
    .state = state::active,
    .requires = {
        capability::presentation,
        capability::observe,
    },
});

auto ui = child->interaction();

auto image = ui.capture();
ui.pointer().click({.fx = 0.5, .fy = 0.5});
ui.keyboard().text("hello");
auto changes = ui.events().subscribe(...);
```

The same call path applies whether `child` is called a tab, panel, dialog,
document, canvas region, or popup.

## Generic events

Core changes describe graph and resource facts, for example:

```cpp
struct ActiveChildChanged {
    NodeRef parent;
    NodeRef previous;
    NodeRef current;
};
```

The generic event vocabulary includes node added/removed, property/state changed,
relation changed, active child changed, focus changed, surface changed, route
changed, and runtime/resource lifecycle changes. Events carry source, provenance,
and affected node/surface identities.

`browser.tab_switched` can remain a compatibility projection at the legacy wire
boundary. It is produced from `ActiveChildChanged` plus node properties when the
legacy client requires it. It is not the event stored or routed inside the new
core.

## Intended public and source structure

The public API is organized around the shared model and operations:

```text
include/grab/
├── session.hpp
├── application.hpp        # convenience discovery/query root, not a subtype tree
├── ui.hpp                 # UiNode, NodeRef, states, and typed properties
├── role.hpp               # canonical role descriptors
├── relation.hpp           # contains/presented_on/occupies/etc.
├── query.hpp              # node selectors and graph queries
├── presentation.hpp       # surfaces, regions, spaces, transforms
├── interaction.hpp        # pointer, keyboard, and semantic operations
├── capture.hpp
├── event.hpp
├── image.hpp
├── capability.hpp
└── result.hpp
```

There is no `browser.hpp`. Accessibility is a semantic source and facet set, not
a separate public object universe.

Protocol and platform specificity is isolated below the model:

```text
src/drivers/
├── desktop/
│   ├── x11/
│   ├── wayland/
│   ├── portal/
│   ├── win32/
│   └── macos/
├── semantic/
│   ├── webextension/
│   │   ├── chromium/
│   │   └── firefox/
│   ├── atspi/
│   ├── uia/
│   └── ax/
└── device/
    ├── evdev/
    └── uinput/
```

These folders classify integration mechanisms. They do not define user-facing
domain types. Later protocol integrations such as CDP or WebDriver BiDi follow the
same semantic-driver boundary; they do not justify a public browser domain.

## Architectural invariants

New work must preserve these rules:

1. No browser, tab, toolkit, or application-specific class is added to the core
   type hierarchy.
2. A new role does not require a new provider interface or event category.
3. Logical containment is never treated as physical presentation.
4. Coordinates are meaningful only with a named space, trusted transform, and
   source information.
5. Capture and physical input routes are selected as a coherent plan; independent
   provider ranking cannot assert coordinate compatibility.
6. Native IDs remain driver aliases. Public node identity is opaque and is never
   inferred authoritatively from title or PID.
7. Object behavior is described by object-scoped capabilities/facets that may
   change at runtime. Unsupported behavior returns a typed error rather than
   empty success.
8. Semantic-to-physical fallback is explicit policy and visible in the operation
   result.
9. Events refer to generic nodes, surfaces, relations, properties, and resources.
10. Legacy browser wire types exist only in compatibility translation.
11. Application-specific landmarks, recipes, fractional target points, and
    selector data stay in caller configuration or application adapters; they
    never enter platform drivers or general library constants.

## Migration from the current port

The existing X11 implementation should be migrated as one vertical workflow
rather than replaced all at once:

1. Introduce node, relation, surface, coordinate-space, query, and route value
   types plus a session-owned graph/store.
2. Make the X11 runtime publish application/window nodes, surfaces, focus,
   topology, capture, input, and changes from its shared connection authority.
3. Reimplement `Screen` and `Input` as compatibility/convenience facades over
   session operations and generic targets; stop accepting raw XIDs publicly.
4. Make AT-SPI contribute semantic facts and actions to the same graph rather
   than producing disconnected app/role/name payloads.
5. Move the browser bridge under a semantic integration driver. Treat the title
   classifier as low-confidence evidence only.
6. Translate generic graph changes to `eventgrab.v1` at the transport boundary,
   retaining `BrowserTab` only as an old-wire payload.
7. Remove canonical browser types after callers have a versioned generic wire/API
   path.

The current `BrowserTab` implementation is therefore migration debt, not an
exception to this decision.

## Later reference-study refinement

The original proposal already used the name `NodeRef`, but did not define its
full ownership and staleness contract. The corpus study preserved the
`UiNode`/surface decision while finding that one
mutable, behavior-heavy, permanently live `UiNode` would become another god
object. The refined representation is:

- `Locator`: immutable and serializable intent;
- `NodeRef`: short-lived, opaque, runtime/tree/epoch/generation-scoped handle;
- `UiTreeView`: explicitly fallible access to current driver data;
- `UiNodeRecord` inside `UiGraphSnapshot`: immutable node facts at a revision;
- `Match`: resolved reference plus evidence and provenance;
- `PinnedTarget`: explicit lease when re-resolution must not substitute identity;
- durable target scopes separated from versioned UI snapshot nodes where their
  lifetimes genuinely differ.

This refinement changes ownership and lifetime mechanics, not the architectural
decision recorded here. Browser, tab, dialog, panel, popup, canvas, and draggable
region still share the same generic node model. Surfaces remain an orthogonal
presentation/resource topology, and runtime drivers still contribute facts and
routes instead of public domain objects.

## Completion test

The architecture is working when a caller can locate an arbitrary child node and
use the same capture, click, type, semantic-action, and watch operations regardless
of whether the node came from X11, AT-SPI, a browser protocol, Wayland, UIA, AX,
or visual evidence—and adding one of those drivers does not add a corresponding
set of browser/tab/window glue paths to the rest of the library.
