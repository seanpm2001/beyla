package otel

import (
	"context"
	"log/slog"
	"sync/atomic"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"

	"github.com/grafana/beyla/pkg/internal/export/attributes"
	"github.com/grafana/beyla/pkg/internal/export/expire"
	"github.com/grafana/beyla/pkg/internal/netolly/ebpf"
)

var timeNow = time.Now

func plog() *slog.Logger {
	return slog.With("component", "otel.Expirer")
}

// Expirer drops metrics from labels that haven't been updated during a given timeout
// TODO: generify and move to a common section for using it also in AppO11y, supporting more OTEL metrics
type Expirer struct {
	attrs   []attributes.Field[*ebpf.Record, string]
	entries *expire.ExpiryMap[*Counter]
}

type Counter struct {
	attributes attribute.Set
	val        atomic.Int64
}

// NewExpirer creates a metric that wraps a Counter. Its labeled instances are dropped
// if they haven't been updated during the last timeout period
func NewExpirer(attrs []attributes.Field[*ebpf.Record, string], clock expire.Clock, expireTime time.Duration) *Expirer {
	return &Expirer{
		attrs:   attrs,
		entries: expire.NewExpiryMap[*Counter](clock, expireTime),
	}
}

// ForRecord returns the Counter for the given eBPF record. If that record
// s accessed for the first time, a new Counter is created.
// If not, a cached copy is returned and the "last access" cache time is updated.
func (ex *Expirer) ForRecord(m *ebpf.Record) *Counter {
	recordAttrs, attrValues := ex.recordAttributes(m)
	return ex.entries.GetOrCreate(attrValues, func() *Counter {
		plog().With("labelValues", attrValues).Debug("storing new metric label set")
		return &Counter{
			attributes: recordAttrs,
		}
	})
}

func (ex *Expirer) Collect(_ context.Context, observer metric.Int64Observer) error {
	log := plog()
	log.Debug("invoking metrics collection")
	old := ex.entries.DeleteExpired()
	log.With("labelValues", old).Debug("deleting old OTEL metric")

	for _, v := range ex.entries.All() {
		observer.Observe(v.val.Load(), metric.WithAttributeSet(v.attributes))
	}

	return nil
}

func (ex *Expirer) recordAttributes(m *ebpf.Record) (attribute.Set, []string) {
	keyVals := make([]attribute.KeyValue, 0, len(ex.attrs))
	vals := make([]string, 0, len(ex.attrs))

	for _, attr := range ex.attrs {
		val := attr.Get(m)
		keyVals = append(keyVals, attribute.String(attr.ExposedName, val))
		vals = append(vals, val)
	}

	return attribute.NewSet(keyVals...), vals
}
