package torrentfs

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	//"path"
	//"io/ioutil"
	"strconv"
	"strings"
	//"sync"
	//"sync/atomic"
	"time"

	"github.com/CortexFoundation/CortexTheseus/common"
	"github.com/CortexFoundation/CortexTheseus/log"
	"github.com/anacrolix/torrent/metainfo"
	bolt "github.com/etcd-io/bbolt"
)

const (
// Chosen to match the usual chunk size in a torrent client. This way,
// most chunk writes are to exactly one full item in bolt DB.
//chunkSize = 1 << 14
)

type FileInfo struct {
	Meta *FileMeta
	// Transaction hash
	TxHash *common.Hash
	// Contract Address
	ContractAddr *common.Address
	LeftSize     uint64
	//Index        uint64
}

//type MutexCounter int32

/*func (mc *MutexCounter) Increase() {
	atomic.AddInt32((*int32)(mc), int32(1))
}

func (mc *MutexCounter) Decrease() {
	atomic.AddInt32((*int32)(mc), int32(-1))
}

func (mc *MutexCounter) IsZero() bool {
	return atomic.LoadInt32((*int32)(mc)) == 0
}*/

type FileStorage struct {
	filesContractAddr map[common.Address]*FileInfo
	files             []*FileInfo
	db                *bolt.DB

	LastListenBlockNumber uint64
	//LastFileIndex         uint64

	//lock      sync.RWMutex
	//bnLock    sync.Mutex
	//opCounter MutexCounter
	dataDir string
	//tmpCache  *lru.Cache
	//indexLock sync.RWMutex
}

var initConfig *Config = nil

//var TorrentAPIAvailable sync.Mutex

func InitConfig() *Config {
	return initConfig
}

func NewFileStorage(config *Config) (*FileStorage, error) {

	if err := os.MkdirAll(config.DataDir, 0700); err != nil {
		return nil, err
	}

	if initConfig == nil {
		initConfig = config
	}

	db, dbErr := bolt.Open(filepath.Join(config.DataDir,
		".file.bolt.db"), 0600, &bolt.Options{
		Timeout: time.Second,
	})
	if dbErr != nil {
		return nil, dbErr
	}
	//db.NoSync = true

	fs := &FileStorage{
		// filesInfoHash:     make(map[metainfo.Hash]*FileInfo),
		filesContractAddr: make(map[common.Address]*FileInfo),
		db:                db,
		//opCounter:         0,
		dataDir: config.DataDir,
	}
	fs.readBlockNumber()
	//fs.readLastFileIndex()
	fs.initFiles()
	//tmpCache, _ := lru.New(120)

	return fs, nil
}

func (fs *FileStorage) Files() []*FileInfo {
	return fs.files
}

func (fs *FileStorage) NewFileInfo(Meta *FileMeta) *FileInfo {
	//ret := &FileInfo{Meta, nil, nil, Meta.RawSize, 0}
	ret := &FileInfo{Meta, nil, nil, Meta.RawSize}
	return ret
}

/*func (fs *FileStorage) AddCachedFile(x *FileInfo) error {
	addr := *x.ContractAddr
	fs.filesContractAddr[addr] = x
	fs.files = append(fs.files, x)
	return nil
}*/

func (fs *FileStorage) CurrentTorrentManager() *TorrentManager {
	return CurrentTorrentManager
}

func (fs *FileStorage) AddFile(x *FileInfo) (uint64, error) {
	addr := *x.ContractAddr
	if _, ok := fs.filesContractAddr[addr]; ok {
		return 0, nil
		//return nil
	}

	//x.Index = fs.LastFileIndex
	//fs.indexLock.Lock()
	//defer fs.indexLock.Unlock()
	//fs.LastFileIndex += 1
	err := fs.WriteFile(x)
	if err != nil {
		//	fs.LastFileIndex -= 1
		return 0, err
	}
	fs.filesContractAddr[addr] = x
	//fs.files = append(fs.files, x)
	return 1, nil
}

func (fs *FileStorage) GetFileByAddr(addr common.Address) *FileInfo {
	if f, ok := fs.filesContractAddr[addr]; ok {
		return f
	}
	return nil
}

// func Exist(infohash string) bool {
// 	TorrentAPIAvailable.Lock()
// 	defer TorrentAPIAvailable.Unlock()
// 	ih := metainfo.NewHashFromHex(infohash[2:])
// 	tm := CurrentTorrentManager
// 	if torrent := tm.GetTorrent(ih); torrent == nil {
// 		return false
// 	} else {
// 		return torrent.IsAvailable()
// 	}
// }

func Available(infohash string, rawSize int64) (bool, error) {
	log.Debug("Available", "infohash", infohash, "RawSize", rawSize)
	//TorrentAPIAvailable.Lock()
	//defer TorrentAPIAvailable.Unlock()
	if !strings.HasPrefix(infohash, "0x") {
		return false, errors.New("invalid info hash format")
	}
	ih := metainfo.NewHashFromHex(infohash[2:])
	tm := CurrentTorrentManager
	//log.Debug("storage", "ih", ih)
	if torrent := tm.GetTorrent(ih); torrent == nil {
		//log.Debug("storage", "ih", ih, "torrent", torrent)
		log.Info("Torrent not found", "hash", infohash)
		return false, errors.New("download not completed")
	} else {
		if !torrent.IsAvailable() {
			log.Info("Download not completed", "hash", infohash, "raw", rawSize)
			return false, errors.New(fmt.Sprintf("download not completed: %d %d", torrent.BytesCompleted(), rawSize))
		}
		//log.Debug("storage", "Available", torrent.IsAvailable(), "torrent.BytesCompleted()", torrent.BytesCompleted(), "rawSize", rawSize)
		//log.Info("download not completed", "complete", torrent.BytesCompleted(), "miss", torrent.BytesMissing(), "raw", rawSize)
		return torrent.BytesCompleted() <= rawSize, nil
	}
}

/*func LoadFile(infohash string, fn string) ([]byte, error) {
        data, err := ioutil.ReadFile(fn)
        return data, err
}*/

/*func GetFile(infohash string, path string) ([]byte, error) {
	infohash = strings.ToLower(infohash)
	//TorrentAPIAvailable.Lock()
	//defer TorrentAPIAvailable.Unlock()
	if strings.HasPrefix(infohash, "0x") {
		//infohash = infohash[2:]
		infohash = infohash[2:]
	}
	ih := metainfo.NewHashFromHex(infohash)
	tm := CurrentTorrentManager
	var torrent Torrent
	if torrent := tm.GetTorrent(ih); torrent == nil {
		log.Debug("storage", "ih", ih, "torrent", torrent)
		fmt.Println("torrent", torrent)
		return nil, errors.New("Torrent not Available: " + infohash)
	}
	data, err := torrent.GetFile(path)
	return data, err
}*/

/*func ExistTorrent(infohash string) bool {
	//TorrentAPIAvailable.Lock()
	//defer TorrentAPIAvailable.Unlock()
	ih := metainfo.NewHashFromHex(infohash[2:])
	tm := CurrentTorrentManager
	if torrent := tm.GetTorrent(ih); torrent == nil {
		return false
	} else {
		return torrent.HasTorrent()
	}
}*/

func (fs *FileStorage) Close() error {
	defer fs.db.Close()
	log.Info("Torrent File Storage Closing")
	// Wait for file storage closed...
	//for {
	//	if fs.opCounter.IsZero() {
	// persist storage block number
	fs.writeBlockNumber()
	//fs.writeLastFileIndex()
	log.Info("Torrent File Storage Closed", "database", fs.db.Path())
	//	}

	// log.Debug("Waiting for boltdb operating...")
	//	time.Sleep(time.Microsecond)
	//}
	return nil
}

var (
	ErrReadDataFromBoltDB = errors.New("Bolt DB Read Error")
)

/*func (fs *FileStorage) GetFileByNumber(index uint64) *FileInfo {
	var info FileInfo

	fs.opCounter.Increase()
	defer fs.opCounter.Decrease()

	cb := func(tx *bolt.Tx) error {
		buk := tx.Bucket([]byte("files"))
		if buk == nil {
			return ErrReadDataFromBoltDB
		}
		k, err := json.Marshal(index)
		if err != nil {
			return ErrReadDataFromBoltDB
		}

		fs.lock.RLock()
		v := buk.Get(k)
		fs.lock.RUnlock()

		if v == nil {
			return ErrReadDataFromBoltDB
		}
		if err := json.Unmarshal(v, &info); err != nil {
			return err
		}

		return nil
	}

	if err := fs.db.View(cb); err != nil {
		return nil
	}
	log.Debug("Read fileinfo from database", "info", info, "meta", info.Meta)
	return &info
}*/

/*func (fs *FileStorage) GetBlockByNumber(blockNum uint64) *Block {
	var block Block

	fs.opCounter.Increase()
	defer fs.opCounter.Decrease()

	cb := func(tx *bolt.Tx) error {
		buk := tx.Bucket([]byte("blocks"))
		if buk == nil {
			return ErrReadDataFromBoltDB
		}
		k, err := json.Marshal(blockNum)
		if err != nil {
			return ErrReadDataFromBoltDB
		}

		//fs.lock.RLock()
		v := buk.Get(k)
		//fs.lock.RUnlock()

		if v == nil {
			return ErrReadDataFromBoltDB
		}
		if err := json.Unmarshal(v, &block); err != nil {
			return err
		}

		return nil
	}

	if err := fs.db.View(cb); err != nil {
		return nil
	}
	return &block
}*/

func (fs *FileStorage) WriteFile(f *FileInfo) error {
	//fs.opCounter.Increase()
	//defer fs.opCounter.Decrease()

	err := fs.db.Update(func(tx *bolt.Tx) error {
		buk, err := tx.CreateBucketIfNotExists([]byte("files"))
		if err != nil {
			return err
		}
		v, err := json.Marshal(f)
		if err != nil {
			return err
		}
		k, err := json.Marshal(f.Meta.InfoHash)
		if err != nil {
			return err
		}

		//fs.lock.Lock()
		e := buk.Put(k, v)
		//fs.lock.Unlock()

		return e
	})

	//if err == nil && b.Number > fs.LastListenBlockNumber {
	//if err == nil {
	//fs.bnLock.Lock()
	//fs.writeLastFileIndex()
	//fs.bnLock.Unlock()
	//}

	return err
}

func (fs *FileStorage) WriteBlock(b *Block) error {
	//fs.opCounter.Increase()
	//defer fs.opCounter.Decrease()

	/*err := fs.db.Update(func(tx *bolt.Tx) error {
		buk, err := tx.CreateBucketIfNotExists([]byte("blocks"))
		if err != nil {
			return err
		}
		v, err := json.Marshal(b)
		if err != nil {
			return err
		}
		k, err := json.Marshal(b.Number)
		if err != nil {
			return err
		}

		//fs.lock.Lock()
		e := buk.Put(k, v)
		//fs.lock.Unlock()

		return e
	})*/

	//if err == nil && b.Number > fs.LastListenBlockNumber {
	//if err == nil {
	//fs.bnLock.Lock()
	fs.LastListenBlockNumber = b.Number
	return fs.writeBlockNumber()
	//fs.bnLock.Unlock()
	//}

	//return err
}

func (fs *FileStorage) initFiles() error {
	return fs.db.View(func(tx *bolt.Tx) error {
		buk := tx.Bucket([]byte("files"))
		if buk == nil {
			return ErrReadDataFromBoltDB
		}

		c := buk.Cursor()

		for k, v := c.First(); k != nil; k, v = c.Next() {

			//		for index := uint64(0); index < fs.LastFileIndex; index++ {
			//			k, err := json.Marshal(index)
			//			if err != nil {
			//				return err
			//			}

			//			v := buk.Get(k)
			//			if v == nil {
			//				return ErrReadDataFromBoltDB
			//			}

			var x FileInfo

			if err := json.Unmarshal(v, &x); err != nil {
				return err
			}
			//fs.indexLock.Lock()
			fs.filesContractAddr[*x.ContractAddr] = &x
			fs.files = append(fs.files, &x)
			//fs.indexLock.Unlock()
		}
		return nil
	})
}

/*func (fs *FileStorage) readLastFileIndex() error {
	return fs.db.View(func(tx *bolt.Tx) error {
		buk := tx.Bucket([]byte("lastFileIndex"))
		if buk == nil {
			return ErrReadDataFromBoltDB
		}

		v := buk.Get([]byte("key"))

		if v == nil {
			return ErrReadDataFromBoltDB
		}

		number, err := strconv.ParseUint(string(v), 16, 64)
		if err != nil {
			return err
		}

		//fs.indexLock.Lock()
		//defer fs.indexLock.Unlock()
		fs.LastFileIndex = number

		return nil
	})
}*/

/*func (fs *FileStorage) writeLastFileIndex() error {
	return fs.db.Update(func(tx *bolt.Tx) error {
		buk, err := tx.CreateBucketIfNotExists([]byte("lastFileIndex"))
		if err != nil {
			return err
		}
		//fs.lock.Lock()
		e := buk.Put([]byte("key"), []byte(strconv.FormatUint(fs.LastFileIndex, 16)))
		//fs.lock.Unlock()

		return e
	})
}*/

func (fs *FileStorage) readBlockNumber() error {
	return fs.db.View(func(tx *bolt.Tx) error {
		buk := tx.Bucket([]byte("currentBlockNumber"))
		if buk == nil {
			return ErrReadDataFromBoltDB
		}

		v := buk.Get([]byte("key"))

		if v == nil {
			return ErrReadDataFromBoltDB
		}

		number, err := strconv.ParseUint(string(v), 16, 64)
		if err != nil {
			return err
		}

		fs.LastListenBlockNumber = number

		return nil
	})
}

func (fs *FileStorage) writeBlockNumber() error {
	return fs.db.Update(func(tx *bolt.Tx) error {
		buk, err := tx.CreateBucketIfNotExists([]byte("currentBlockNumber"))
		if err != nil {
			return err
		}
		//fs.lock.Lock()
		e := buk.Put([]byte("key"), []byte(strconv.FormatUint(fs.LastListenBlockNumber, 16)))
		//fs.lock.Unlock()

		return e
	})
}

type FlowControlMeta struct {
	InfoHash       metainfo.Hash
	BytesRequested uint64
	IsCreate       bool
}
