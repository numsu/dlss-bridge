# Launch wrappers

The wrappers invoke the same controller on Linux/Proton and native Windows.
The controller prepares an isolated session in user state and does not write to
the game directory or Wine prefix. Launch context does not change capture,
scheduling, execution, or failure policy.
