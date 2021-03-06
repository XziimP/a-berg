package main

import (
	"beam.mw/service-balancer/services"
	"errors"
	"github.com/capnm/sysinfo"
	syscall "golang.org/x/sys/unix"
	"log"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"runtime/debug"
)

type WalletStats struct {
	services.ServiceStats
	EndpointsCnt int
	ClientsCnt int
}

type DiskStatus struct {
	AllGB   float64
	UsedGB  float64
	FreeGB  float64
	AvailGB float64
}

type DBStatus struct {
	SizeMB	float64
	SizeGB  float64
}

type statusRes struct {
	GoMemory          runtime.MemStats
	SysInfo			  *sysinfo.SI
	DBDiskTotal       uint64
	DBDiskFree        uint64
	GC                debug.GCStats
	NumCPU            int
	NumGos            int
	MaxWalletServices int
	MaxBbsServices    int
	WalletServices    []*WalletStats
	BbsServices       []*services.ServiceStats
	Config            *Config
	Counters          Counters
	WalletSockets     int64
	DBSize            DBStatus
	DBDiskUsage       DiskStatus
	SelfDiskUsage     DiskStatus
}

func statusRequest (r *http.Request)(interface{}, error) {
	if len(config.APISecret) == 0 && !config.Debug {
		return nil, errors.New("no secret provided in config")
	}

	if len(config.APISecret) != 0 {
		if r.FormValue("secret") != config.APISecret {
			return nil, errors.New("bad access token")
		}
	}

	res := &statusRes {
		NumCPU:     runtime.NumCPU(),
		NumGos:     runtime.NumGoroutine(),
		Config:     &config,
	}

	// Do not expose sensitive info
	res.Config.VAPIDPrivate = "--not exposed--"
	counters.CopyTo(&res.Counters)
	res.WalletSockets = res.Counters.WConnect - res.Counters.WDisconnect

	runtime.ReadMemStats(&res.GoMemory)
	res.SysInfo = sysinfo.Get()
	debug.ReadGCStats(&res.GC)

	wstats := walletServices.GetStats()
	if len(wstats) != 0 {
		res.WalletServices = make([]*WalletStats, len(wstats))
		for i, stat := range wstats {
			full := WalletStats{}
			full.Port = stat.Port
			full.Pid = stat.Pid
			full.Args = stat.Args
			full.ProcessState = stat.ProcessState
			full.EndpointsCnt, full.ClientsCnt = epoints.GetSvcCounts(i)
			res.WalletServices[i] = &full
		}
	}

	res.BbsServices       = sbbsServices.GetStats()
	res.MaxWalletServices = len(res.WalletServices)
	res.MaxBbsServices    = len(res.BbsServices)
	res.DBSize            = DBSize(config.DatabasePath)
	res.DBDiskUsage       = DiskUsage(config.DatabasePath)
	res.SelfDiskUsage     = DiskUsage("./")

	return res, nil
}

const (
	B  = 1
	KB = 1024 * B
	MB = 1024 * KB
	GB = 1024 * MB
)

func BytesTo(bytes uint64, to uint64) float64 {
	return math.Round(float64(bytes) / float64(to) * 100) / 100
}

func DiskUsage(path string) (disk DiskStatus) {
	fs := syscall.Statfs_t{}
	err := syscall.Statfs(path, &fs)
	if err != nil {
		return
	}

	disk.AllGB   = BytesTo(fs.Blocks * uint64(fs.Bsize), GB)
	disk.AvailGB = BytesTo(fs.Bavail * uint64(fs.Bsize), GB)
	disk.FreeGB  = BytesTo(fs.Bfree * uint64(fs.Bsize),  GB)
	disk.UsedGB  = disk.AllGB - disk.FreeGB

	return
}

func DBSize(path string) DBStatus {
	var size uint64
	err := filepath.Walk(path, func(_ string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			size += uint64(info.Size())
		}
		return err
	})

	if err != nil {
		log.Printf("error in DBSize %v", err)
		return DBStatus{}
	} else {
		status := DBStatus{
			SizeGB: BytesTo(size, GB),
			SizeMB: BytesTo(size, MB),
		}
		return status
	}
}
