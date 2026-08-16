---
name: prd-implementation-audit
description: Audit a PRD against its implementation and review diff for missing behavior, false-complete or opportunistic implementations, contract violations, async/event-loop blocking, repeated syscall or IPC overhead, capability and permission flaws, lifecycle/resource leaks, backpressure mistakes, and stale acceptance claims. Use when reviewing a PR, feature branch, migration, kernel/userland boundary, IDL/API implementation, or when the user asks how many issues remain, whether the implementation matches the PRD, or asks to record fixes without coding.
---

# PRD Implementation Audit

Review the stated product behavior and the actual implementation as one traceable system. Produce evidence-backed findings; do not infer completion from comments, green unit tests, or an implementation-status document alone.

## Operating rules

- Read repository instructions and identify the PRD, acceptance criteria, changed files, generated code, IDL, and relevant integration paths before judging behavior.
- Preserve the worktree. Do not rewrite code, generated artifacts, submodules, or user changes unless the user explicitly asks for implementation.
- If the user says not to run tests, do not run tests, builds, benchmarks, QEMU, or other execution checks. Static inspection, `rg`, `git diff`, and reading existing logs are allowed when useful; label old evidence as stale when its build state is unknown.
- If the user asks to record TODOs only, record decisions and recommended changes without implementing them. If they ask to review one issue at a time, show only the next unresolved issue and wait for confirmation.
- Prefer exact file-and-line evidence. Separate confirmed defects from risks, design debts, stale documentation, and assumptions.
- Count distinct root causes, not every call site. Mention affected call sites under the same finding.

## Subagent review policy

- For substantive review or audit work, use at least one independent subagent by default.
- Prefer parallel, independent passes for PRD/acceptance traceability, async/IPC/performance and event-loop behavior, and capability/permission/lifecycle/error cleanup.
- Give each subagent the relevant raw artifacts and review question. Do not preload the suspected finding or desired conclusion.
- Require each subagent to return the finding, exact file-and-line evidence, impact, confidence, and any unresolved gap. Keep subagent reviews read-only; they must not modify the worktree.
- Keep final responsibility in the main agent: reconcile contradictions, verify evidence, deduplicate findings by root cause, assign severity, and produce the final synthesis.
- Use a single-agent review only when the task is trivial, the user explicitly forbids subagents, the environment does not provide them, or parallelization would add disproportionate overhead; state that exception in the report.
- Never invent subagent results or imply that an independent pass occurred when it did not.

## Audit workflow

### 1. Establish the contract

Extract every PRD requirement into a compact traceability table:

| ID | Requirement | Acceptance evidence | Implementation locations | Status |
|---|---|---|---|---|

Include explicit non-goals, compatibility boundaries, error semantics, ownership rules, concurrency requirements, resource limits, and failure/restart behavior. Treat “must”, “shall”, “only”, “never”, “async”, “atomic”, “single owner”, and “no extra syscall” as testable constraints.

Resolve contradictions between the PRD, ADRs, TODOs, generated interfaces, and the user's later decisions. State which source is authoritative instead of silently choosing one.

### 2. Map the implementation path

For each requirement, follow the whole path:

1. Public API or IDL declaration.
2. Client or compatibility-layer wrapper.
3. Transport/invocation and capability transfer.
4. Server dispatcher or event loop.
5. State machine, kernel object, filesystem, or device operation.
6. Completion, cancellation, peer-close, cleanup, and retry path.

Use `rg`/`rg --files` first. Inspect generated bindings when resource indexes, revisions, bindings, protocol rights, async behavior, or response counts are involved; generated code is part of the contract, not disposable noise.

### 3. Detect false completion and opportunistic code

Flag an implementation as incomplete when it:

- accepts an argument but ignores it, returns success without observable behavior, or stores state that no consumer reads;
- exposes a method in IDL/header but routes it to a no-op, generic fallback, `ENOTTY`, `ENOSYS`, or unconditional success contrary to the PRD;
- validates only the happy path and omits resource-shape, ownership, rights, rollback, cancellation, peer-close, or generation checks;
- checks a condition in one layer and performs the operation later without an atomic/linearizable boundary;
- moves a requirement to a weaker layer merely to make a test pass, such as trusting a caller PID, URI, numeric ID, or transport signal;
- claims an async operation while synchronously waiting inside the service's single event loop;
- supports only a narrow demo path, mock/serial path, or boot path while the real path is unimplemented;
- updates a status document but leaves source behavior or acceptance criteria unchanged.

Comments are evidence of intent only. Code paths and observable state determine status.

### 4. Review async and syscall performance explicitly

For every read/write/control operation, count the actual submit, wait, take, retry, readiness, and job-control calls. Distinguish:

- a normal async invocation lifecycle (`submit -> later wait -> take`), which is expected when the caller is asynchronous;
- an accidental nested synchronous wait inside a server event loop, which can block unrelated clients and cause head-of-line blocking;
- a separate pre-check RPC followed by the real I/O RPC, which can create a race and extra IPC/syscall cost;
- a query-per-fd plus watch design that is required by the readiness protocol versus unnecessary repeated queries.

Check that:

- read/write and authorization are atomic where the PRD requires it;
- short writes preserve and drain the unwritten suffix;
- backpressure queues are bounded, ordered, and resumed by readiness rather than blocking the dispatcher;
- pending responders can be canceled and are completed exactly once;
- event loops wait on completion/readiness signals, not on a transport signal that has different semantics.

Common red flags include a `check_io()` RPC before every read/write, `_na_handle_wait_many()` in a server handler, a master client waiting for `NA_SIGNAL_READABLE` when the protocol endpoint only exposes transport writable state, and ignored response counters.

### 5. Review capability, permission, and lifecycle boundaries

Verify every transferred resource by binding, scope, UUID, revision, protocol rights, metadata rights, resource index, and expected count. Check both success and all failure returns.

Audit:

- duplicate vs move semantics and rights attenuation;
- server/client/kernel-view binding confusion;
- namespace and service registration authority;
- current-caller identity versus user-supplied PID/group/ID;
- object revocation and controlling-terminal/session lifetime;
- close, fork, exec, exit, mmap/munmap, peer-close, restart, and crash cleanup;
- spinlocks held across allocation, IPC, capability-table operations, callbacks, or other complex code.

Never accept “the capability is only given to a trusted service” as the complete namespace or authority policy. Identify what that capability can actually do after duplication or transfer.

### 6. Reconcile the result

Before reporting, classify each requirement as:

- **Implemented** — code path and contract match;
- **Partially implemented** — happy path exists but an explicit requirement is missing;
- **Broken** — reachable path contradicts the contract;
- **Unverified** — static evidence is insufficient and execution was not authorized;
- **Out of scope** — explicitly excluded by the PRD.

Do not count an already fixed issue twice. If previous evidence conflicts with current source, report the current source as authoritative and mark old logs/builds as stale.

## Reporting format

Lead with the total, then split findings by severity:

- **Blocker / P0**: prevents required boot, core API behavior, safety, or acceptance.
- **High / P1**: security, data loss, deadlock, lifecycle, or major PRD violation.
- **Medium / P2**: incomplete edge behavior, resource leak, compatibility gap, or significant performance debt.
- **Low / P3**: maintainability, documentation, or bounded optimization.

For every finding provide:

1. Short title and severity.
2. Exact evidence with clickable absolute file links and line numbers when available.
3. Why it violates the PRD or creates a concrete failure mode.
4. Minimal recommended direction, without implementing unless requested.

End with:

- confirmed remaining count;
- issues explicitly fixed and therefore not counted;
- requirements still unverified because execution was skipped;
- whether the PRD can be accepted as-is.

When the user wants a TODO document, include acceptance criteria and recommended implementation direction for each item. Acceptance criteria must be observable, for example: “when the queue is full, the event loop remains able to process peer-close and input events; the pending write completes with the full count after capacity returns.”
