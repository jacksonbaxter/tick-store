"""The comparison systems, behind one interface. Phase 3 adds tick-store's own."""


def system_class(name):
    """Imported on demand, so that a process only ever loads the one engine it runs."""
    if name == "parquet":
        from compare.systems.parquet_system import ParquetSystem
        return ParquetSystem
    if name == "arcticdb":
        from compare.systems.arcticdb_system import ArcticDBSystem
        return ArcticDBSystem
    if name == "questdb":
        from compare.systems.questdb_system import QuestDBSystem
        return QuestDBSystem
    raise ValueError(f"unknown system {name}")


SYSTEM_NAMES = ("parquet", "arcticdb", "questdb")
