// Package beyla provides public access to Beyla as a library. All the other subcomponents
// of Beyla are hidden.
package beyla

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/grafana/beyla/pkg/beyla/config"
	"github.com/grafana/beyla/pkg/internal/connector"
	"github.com/grafana/beyla/pkg/internal/discover"
	"github.com/grafana/beyla/pkg/internal/imetrics"
	"github.com/grafana/beyla/pkg/internal/pipe"
	"github.com/grafana/beyla/pkg/internal/pipe/global"
	"github.com/grafana/beyla/pkg/internal/request"
	"github.com/grafana/beyla/pkg/internal/transform/kube"
)

func log() *slog.Logger {
	return slog.With("component", "beyla.Instrumenter")
}

// Instrumenter finds and instrument a service/process, and forwards the traces as
// configured by the user
type Instrumenter struct {
	config  *config.Config
	ctxInfo *global.ContextInfo

	// tracesInput is used to communicate the found traces between the ProcessFinder and
	// the ProcessTracer.
	// TODO: When we split beyla into two executables, probably the BPF map
	// should be the traces' communication mechanism instead of a native channel
	tracesInput chan []request.Span
}

// New Instrumenter, given a Config
func New(config *config.Config) *Instrumenter {
	return &Instrumenter{
		config:      config,
		ctxInfo:     buildContextInfo(config),
		tracesInput: make(chan []request.Span, config.ChannelBufferLen),
	}
}

// FindAndInstrument searches in background for any new executable matching the
// selection criteria.
func (i *Instrumenter) FindAndInstrument(ctx context.Context) error {
	finder := discover.NewProcessFinder(ctx, i.config, i.ctxInfo)
	foundProcesses, err := finder.Start(i.config)
	if err != nil {
		return fmt.Errorf("couldn't start Process Finder: %w", err)
	}
	// In background, listen indefinitely for each new process and run its
	// associated ebpf.ProcessTracer once it is found.
	go func() {
		for {
			select {
			case <-ctx.Done():
				log().Debug("stopped searching for new processes to instrument")
				return
			case pt := <-foundProcesses:
				go pt.Run(ctx, i.tracesInput)
			}
		}
	}()
	// TODO: wait until all the resources have been freed/unmounted
	return nil
}

// ReadAndForward keeps listening for traces in the BPF map, then reads,
// processes and forwards them
func (i *Instrumenter) ReadAndForward(ctx context.Context) error {
	log := log()
	log.Debug("creating instrumentation pipeline")

	// TODO: when we split the executable, tracer should be reconstructed somehow
	// from this instance
	bp, err := pipe.Build(ctx, i.config, i.ctxInfo, i.tracesInput)
	if err != nil {
		return fmt.Errorf("can't instantiate instrumentation pipeline: %w", err)
	}

	log.Info("Starting main node")

	bp.Run(ctx)

	log.Info("exiting auto-instrumenter")

	return nil
}

// buildContextInfo populates some globally shared components and properties
// from the user-provided configuration
func buildContextInfo(config *config.Config) *global.ContextInfo {
	promMgr := &connector.PrometheusManager{}
	k8sCfg := &config.Attributes.Kubernetes
	ctxInfo := &global.ContextInfo{
		ReportRoutes:  config.Routes != nil,
		Prometheus:    promMgr,
		K8sDecoration: k8sCfg.Enabled(),
	}
	if ctxInfo.K8sDecoration {
		// Creating a common Kubernetes database that needs to be accessed from different points
		// in the Beyla pipeline
		var err error
		if ctxInfo.K8sDatabase, err = kube.StartDatabase(k8sCfg.KubeconfigPath, k8sCfg.InformersSyncTimeout); err != nil {
			slog.Error("can't setup Kubernetes database. Your traces won't be decorated with Kubernetes metadata",
				"error", err)
			ctxInfo.K8sDecoration = false
		}

	}
	if config.InternalMetrics.Prometheus.Port != 0 {
		slog.Debug("reporting internal metrics as Prometheus")
		ctxInfo.Metrics = imetrics.NewPrometheusReporter(&config.InternalMetrics.Prometheus, promMgr)
		// Prometheus manager also has its own internal metrics, so we need to pass the imetrics reporter
		// TODO: remove this dependency cycle and let prommgr to create and return the PrometheusReporter
		promMgr.InstrumentWith(ctxInfo.Metrics)
	} else {
		slog.Debug("not reporting internal metrics")
		ctxInfo.Metrics = imetrics.NoopReporter{}
	}
	return ctxInfo
}
