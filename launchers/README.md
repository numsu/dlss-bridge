# Launch frontends

Every frontend consumes the same resolved manifest. Desktop, Steam/Proton,
Sunshine, and Windows integrations may prepare environment variables and mount
or copy a game-local bundle, but must not alter capture, scheduling, execution,
or fallback policy. Sunshine is a launcher/capture frontend and is not part of
the in-process frame path.
