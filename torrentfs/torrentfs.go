package torrentfs

import (
	"fmt"
	"github.com/CortexFoundation/CortexTheseus/log"
	"github.com/CortexFoundation/CortexTheseus/p2p"
	"github.com/CortexFoundation/CortexTheseus/params"
	"github.com/CortexFoundation/CortexTheseus/rpc"
	"io/ioutil"
	"path"
	"sync"
)

type CVMStorage interface {
	Available(infohash string, rawSize int64) (bool, error)
	GetFile(infohash string, path string) ([]byte, error)
	Stop() error
}
type GeneralMessage struct {
	Version string `json:"version,omitempty"`
	Commit  string `json:"commit,omitempty"`
}

// TorrentFS contains the torrent file system internals.
type TorrentFS struct {
	config  *Config
	history *GeneralMessage
	monitor *Monitor

	fileLock sync.Mutex
}

func (t *TorrentFS) Config() *Config {
	return t.config
}

func (t *TorrentFS) Monitor() *Monitor {
	return t.monitor
}

var torrentInstance *TorrentFS = nil

func GetTorrentInstance() *TorrentFS {
	if torrentInstance == nil {
		torrentInstance, _ = New(&DefaultConfig, "")
	}
	return torrentInstance
}

func GetStorage() CVMStorage {
	return GetTorrentInstance()
}

func GetConfig() *Config {
	if torrentInstance != nil {
		return GetTorrentInstance().Config()
	} else {
		return &DefaultConfig
	}
	return nil
}

// New creates a new dashboard instance with the given configuration.
var Torrentfs_handle CVMStorage

// New creates a new torrentfs instance with the given configuration.
func New(config *Config, commit string) (*TorrentFS, error) {
	if torrentInstance != nil {
		return torrentInstance, nil
	}

	versionMeta := ""
	//TorrentAPIAvailable.Lock()
	if len(params.VersionMeta) > 0 {
		versionMeta = fmt.Sprintf(" (%s)", params.VersionMeta)
	}

	msg := &GeneralMessage{
		Commit:  commit,
		Version: fmt.Sprintf("v%d.%d.%d%s", params.VersionMajor, params.VersionMinor, params.VersionPatch, versionMeta),
	}

	monitor, moErr := NewMonitor(config)
	if moErr != nil {
		log.Error("Failed create monitor")
		return nil, moErr
	}

	torrentInstance = &TorrentFS{
		config:  config,
		history: msg,
		monitor: monitor,
	}
	Torrentfs_handle = torrentInstance

	return torrentInstance, nil
}

// Protocols implements the node.Service interface.
func (tfs *TorrentFS) Protocols() []p2p.Protocol { return nil }

// APIs implements the node.Service interface.
func (tfs *TorrentFS) APIs() []rpc.API { return nil }

// Start starts the data collection thread and the listening server of the dashboard.
// Implements the node.Service interface.
func (tfs *TorrentFS) Start(server *p2p.Server) error {
	log.Info("Torrent monitor starting", "torrentfs", tfs)
	if tfs == nil || tfs.monitor == nil {
		return nil
	}
	return tfs.monitor.Start()
}

// Stop stops the data collection thread and the connection listener of the dashboard.
// Implements the node.Service interface.
func (tfs *TorrentFS) Stop() error {
	// Wait until every goroutine terminates.
	tfs.monitor.Stop()
	return nil
}

func (fs *TorrentFS) Available(infohash string, rawSize int64) (bool, error) {
	// modelDir := fs.DataDir + "/" + infoHash
	// if (os.Stat)
	return Available(infohash, rawSize)
}

func (fs *TorrentFS) GetFile(infohash string, subpath string) ([]byte, error) {
	fs.fileLock.Lock()
	defer fs.fileLock.Unlock()
	fn := path.Join(fs.config.DataDir, infohash, subpath)
	data, err := ioutil.ReadFile(fn)
	return data, err
	//return LoadFile(infohash, fn)
	//return GetFile(infohash, subpath)
}
