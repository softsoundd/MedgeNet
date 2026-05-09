package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"medgenet-prototype/config"
	"medgenet-prototype/fesl"
	"medgenet-prototype/locker"
	"medgenet-prototype/mlog"
	"medgenet-prototype/storage"
)

func main() {
	cfg := config.Load()
	log, err := mlog.New(cfg.LogPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to initialize log: %v\n", err)
		os.Exit(1)
	}
	defer log.Close()

	store, err := storage.Open(cfg.DBPath)
	if err != nil {
		log.Write("INIT", "failed to open database: "+err.Error())
		os.Exit(1)
	}
	defer store.Close()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	log.Write("INIT", fmt.Sprintf("MedgeNet Go server starting on %s (FESL %d, HTTP %d)", cfg.BindHost, cfg.FESLPort, cfg.HTTPPort))
	log.Write("INIT", fmt.Sprintf("DB=%s GhostDir=%s LockerURL=%s", cfg.DBPath, cfg.GhostDir, cfg.LockerURL))

	errs := make(chan error, 2)
	go func() { errs <- fesl.NewServer(cfg, log, store).Listen(ctx) }()
	go func() { errs <- locker.NewServer(cfg, log, store).Listen(ctx) }()

	err = <-errs
	if err != nil && ctx.Err() == nil {
		log.Write("INIT", "server error: "+err.Error())
		os.Exit(1)
	}
	log.Write("INIT", "Server stopped")
}
