# Imported by every Python process the AI Signal Lab's agent starts
# (kraken_ai.py puts this folder on PYTHONPATH): caps its memory so a script
# that would need more than the Pi has free fails with MemoryError instead
# of swapping the receiver to a standstill. See kraken_memguard.py.
# (Takes the place of the distribution's sitecustomize for these processes;
# Debian's only installs an apport hook.)
try:
    import kraken_memguard
    kraken_memguard.apply()
except Exception:
    pass
