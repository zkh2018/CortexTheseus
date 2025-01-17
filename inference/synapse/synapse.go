package synapse

import (
	"fmt"
	"github.com/CortexFoundation/CortexTheseus/common/lru"
	"github.com/CortexFoundation/CortexTheseus/inference/synapse/kernel"
	"github.com/CortexFoundation/CortexTheseus/log"
	"github.com/CortexFoundation/CortexTheseus/torrentfs"
	"strconv"
	"sync"
)

var synapseInstance *Synapse = nil

const PLUGIN_PATH string = "plugins/"
const PLUGIN_POST_FIX string = "_cvm.so"

const MinMemoryUsage int64 = 2 * 1024 * 1024 * 1024
const ReservedMemoryUsage int64 = 512 * 1024 * 1024

type Config struct {
	// StorageDir    string `toml:",omitempty"`
	IsNotCache     bool   `toml:",omitempty"`
	DeviceType     string `toml:",omitempty"`
	DeviceId       int    `toml:",omitempty"`
	IsRemoteInfer  bool   `toml:",omitempty"`
	InferURI       string `toml:",omitempty"`
	Debug          bool   `toml:",omitempty"`
	MaxMemoryUsage int64
	Storagefs      torrentfs.CVMStorage
}

var DefaultConfig Config = Config{
	// StorageDir:    "",
	IsNotCache:     false,
	DeviceType:     "cpu",
	DeviceId:       0,
	IsRemoteInfer:  false,
	InferURI:       "",
	Debug:          false,
	MaxMemoryUsage: 4 * 1024 * 1024 * 1024,
}

type Synapse struct {
	config      *Config
	simpleCache sync.Map
	modelLock   sync.Map
	lib         *kernel.LibCVM
	caches      map[int]*lru.Cache
	exitCh      chan struct{}
}

func Engine() *Synapse {
	if synapseInstance == nil {
		log.Error("Synapse Engine has not been initalized")
		return New(&DefaultConfig)
	}

	return synapseInstance
}

func New(config *Config) *Synapse {
	path := PLUGIN_PATH + config.DeviceType + PLUGIN_POST_FIX
	if synapseInstance != nil {
		log.Warn("Synapse Engine has been initalized")
		if config.Debug {
			fmt.Println("Synapse Engine has been initalized")
		}
		return synapseInstance
	}
	var lib *kernel.LibCVM
	var status int
	if !config.IsRemoteInfer {
		lib, status = kernel.LibOpen(path)
		if status != kernel.SUCCEED {
			log.Error("infer helper", "init cvm plugin error", "")
			if config.Debug {
				fmt.Println("infer helper", "init cvm plugin error", "")
			}
			return nil
		}
		if lib == nil {
			panic("lib_path = " + PLUGIN_PATH + config.DeviceType + PLUGIN_POST_FIX + " config.IsRemoteInfer = " + strconv.FormatBool(config.IsRemoteInfer))
		}
	}

	synapseInstance = &Synapse{
		config: config,
		lib:    lib,
		exitCh: make(chan struct{}),
		caches: make(map[int]*lru.Cache),
	}

	log.Info("Initialising Synapse Engine", "Cache Disabled", config.IsNotCache)
	return synapseInstance
}

func (s *Synapse) Close() {
	close(s.exitCh)
	if s.config.Storagefs != nil {
		s.config.Storagefs.Stop()
	}
	log.Info("Synapse Engine Closed")
}

func (s *Synapse) InferByInfoHash(modelInfoHash, inputInfoHash string) ([]byte, error) {
	if s.config.IsRemoteInfer {
		return s.remoteInferByInfoHash(modelInfoHash, inputInfoHash)
	}
	return s.inferByInfoHash(modelInfoHash, inputInfoHash)
}

func (s *Synapse) InferByInputContent(modelInfoHash string, inputContent []byte) ([]byte, error) {
	if s.config.IsRemoteInfer {
		return s.remoteInferByInputContent(modelInfoHash, inputContent)
	}
	inputInfoHash := RLPHashString(inputContent)
	return s.inferByInputContent(modelInfoHash, inputInfoHash, inputContent)
}

func (s *Synapse) GetGasByInfoHash(modelInfoHash string) (gas uint64, err error) {
	if s.config.IsRemoteInfer {
		return s.remoteGasByModelHash(modelInfoHash)
	}
	return s.getGasByInfoHash(modelInfoHash)
}
