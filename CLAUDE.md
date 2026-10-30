## Model
- Every session, including you and each project session, runs on Opus 5.5 (claude-opus-5-5) at xhigh effort.
- Every subagent runs on Opus 5.5 at high effort. Whenever you or a session spawns a subagent, set its effort to high. In any subagent definition you write, put model: claude-opus-5-5 and effort: high in its frontmatter.
- This folder's .claude/settings.local.json enforces the model. Don't add an effort setting to that file. Set effort per session and per subagent as above.

## When to stop
When a step doesn't need my input, keep going, and put status notes in the same message as your next action.

Stop and ask only:
- when you can't continue without me,
- before anything destructive or outside these folders.
- if any session or subagent turns out to be running on a model or effort other than the ones in the Model section. Don't work around it; tell me which one and what it ran on.

If the environment can't produce a required measurement (no perf counters, no CPU pinning), or no free, redistributable option-chain snapshot can be found, don't estimate or substitute. Keep working on everything else, and list the gap under Blocked on me.
