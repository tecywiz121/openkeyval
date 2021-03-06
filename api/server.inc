<?php

  global $CONFIG;
  require_once('../config.inc');

  class OpenKeyval {
    const kMaxDataSize = 65536;
    const kReadOnlyKeyPrefix = 'rok-';
    const kWriteOnceKeyPrefix = 'wok-';

    public static function Dispatch() {
      self::fixMagicQuotesIfNeccessary();
            
      if ($_SERVER['REQUEST_METHOD'] == 'HEAD') {
        $_SERVER['REQUEST_METHOD'] = 'GET';
        $GLOBALS['WAS_HEAD'] = true;
      }

      if($_SERVER['HTTP_HOST'] != $GLOBALS['CONFIG']['api_hostname']) {
        self::Response(400, array('error' => 'Hostname mismatch; perhaps you are mixing SSL and non-SSL hostnames?'));
        exit;
      } 

      if($_SERVER['REQUEST_METHOD'] == 'GET' && $_SERVER['REQUEST_URI'] == '/') {
        self::Response(400, array('error' => 'no_key_specified'));
        exit;
      }

      if($_SERVER['REQUEST_METHOD'] == 'POST' && $_SERVER['REQUEST_URI'] == '/') {
        foreach ($_POST as $key=>$value) {
          if(!self::IsValidKey($key)) {
            self::Response(400, array('error' => 'invalid_key', 'key' => $key));
          }
        }

        $key_map = array();
        foreach ($_POST as $key=>$value) {
          self::throttle("write");
          self::HandlePOST($key,$value);
          $key_map[$key] = self::ReadOnlyKey($key);
        }
        self::Response(200, array('status' => 'multiset', 'keys' => $key_map));
      }

      if($_REQUEST['key'] == 'store/') {
        unset($_REQUEST['key']);
        unset($_GET['key']);
        unset($_POST['key']);
      }

      if (isset($_REQUEST['key'])) {
        if(strpos($_REQUEST['key'], '.')) {
          list($key, $command) = explode('.', $_REQUEST['key']);
        } else {
          $key = $_REQUEST['key'];
          $command = '';
        }
      }

      if (strpos($_SERVER['REQUEST_URI'],'/store/')===0) {
        self::HandleBastardSonOfAProtocolJSONP();
      }

      if ($_SERVER['REQUEST_METHOD'] == 'OPTIONS') {
        self::HandleOPTIONS();
      }

      if(!self::IsValidKey($key)) {
        self::Response(400, array('error' => 'invalid_key'));
      }

      if (isset($_REQUEST['key_info'])) {
        # Mainly used for debugging, but also provides a way to lookup a the read-only key for a given key (without setting)
        if(self::IsReadOnlyKey($key)) {
          self::Response(200, array('read_only_key' => $key));
        } else {
          self::Response(200, array('key' => $key,
                                    'hash' => self::HashForKey($key),
                                    'read_only_key' => self::ReadOnlyKey($key),
                                    'is_write_once' => self::IsWriteOnceKey($key)));
        }
      }


      if($_SERVER['REQUEST_METHOD'] == 'GET') {
        self::throttle("read");
        self::HandleGET($key, $command);
      } else {
        self::throttle("write");
        self::HandlePOST($key, $_POST['data']);
        self::Response(200, array('status' => 'set', 'key' => $key, 'read_only_key' => self::ReadOnlyKey($key)));
      }
    }
    
    private function throttle($type="read") {
      
      $ip = $_SERVER['REMOTE_ADDR']; 
      
      # Dirty hack for time being
      if ($ip == "184.72.10.139") {
        return;
      }

      $currentTimeChunk = ceil(time()/60/60)*60*60;
      $key = 'internal-throttle-' . $GLOBALS['CONFIG']['api_hostname'] . "-" . $ip . '-' . '-' . $type . '-' . $currentTimeChunk;
      
      $memcache = GetMemcacheHandle();
      
      if ($memcache !== false) {
      
        $currentLimit = $memcache->get($key)*1;
      
        if ($type == "read") {
          $maxLimit = 1000;
        } else {
          $maxLimit = 100;
        }        

        $expire = $currentTimeChunk-time();

        $memcache->set($key,$currentLimit+1,false,$expire);

        if ($currentLimit>$maxLimit) {
          self::Response(400, array('error' => 'API usage limit exceeded; exceeded hourly quota','throttleType'=>$type,'requestsInPeriodSoFar'=>$currentLimit,'quota'=>$maxLimit,'periodCountdown'=>$expire));
        }
      }
    }

    private function fixMagicQuotesIfNeccessary() {
      if (get_magic_quotes_gpc()) {
        function strip_array($var) {
          return is_array($var) ? array_map("strip_array", $var) : stripslashes($var);
        }
        $_POST = strip_array($_POST);
        $_SESSION = strip_array($_SESSION);
        $_GET = strip_array($_GET);
      }
    }

    private function determineJSONPCallback() {
      $jsonp = null;
      foreach (array("jsonp_callback","callback") as $k) {
        if (isset($_GET[$k])) {
          $jsonp = $k;
        }
      }
      return $jsonp;
    }


    public static function HandleBastardSonOfAProtocolJSONP() {
      $set = array();
      // Pretend GET params were each a POST
      $key_map = array();
      foreach ($_GET as $key=>$value) {
        if ($key != self::determineJSONPCallback()) {
          self::throttle("write");        
          self::HandlePOST($key, $value);
          $key_map[$key] = self::ReadOnlyKey($key);
        }
      }
      self::Response(200, array('status' => 'multiset', 'keys' => $key_map));
    }

    public static function HandleGET($key, $command) {
      $value = OpenKeyval_Storage_Cached::Get($key);
      if($value !== NULL) {
        if(strpos($command, '/')) {
          $content_type = $command;
          header('Content-Disposition: filename="' . $key . '"');
        } else {
          $content_type = 'text/plain';
        }
        self::Response(200, $value, $content_type, $key);
      }

      self::Response(404, array('error' => 'not_found'));
    }

    public static function HandleOPTIONS() {
      header("HTTP/1.0 200 OK");
      header("Allow: OPTIONS,GET,POST");
      exit;
    }

    public static function HandlePOST($key, $value) {
      if(!isset($value)) {
        self::Response(400, array('error' => 'missing_field', 'message' => "Data must be sent as form data in the field 'data'"));
        return false;
      }

      if(strlen($value) > self::kMaxDataSize) {
        self::Response(413, array('error' => 'data too big, max length is ' . self::kMaxDataSize . ' bytes'));
        return false;
      }

      if($value === '') {
        if(OpenKeyval_Storage_Cached::Get($key)) {
          if (OpenKeyval_Storage_Cached::Delete($key)) {
            self::Response(200, array('status' => 'deleted'));
          } else {
            self::Response(403, array('status' => 'delete_failed'));
          }
          return false;
        } else {
          self::Response(200, array('status' => 'did_not_exist'));
          return false;
        }
      }

      if(OpenKeyval_Storage_Cached::Set($key, $value)) {
        return true;
      } else {
        self::Response(500, array('error' => 'save_failed'));
        return false;
      }
    }

    public static function IsReadOnlyKey($key) {
      return (strpos($key, OpenKeyval::kReadOnlyKeyPrefix) === 0);
    }
    
    public static function IsWriteOnceKey($key) {
      return (strpos($key, OpenKeyval::kWriteOnceKeyPrefix) === 0);
    }

    public static function IsValidKey($key) {
      if(self::IsReadOnlyKey($key)) {
        $key = substr($key, strlen(self::kReadOnlyKeyPrefix));
        return !!preg_match('/^[a-z0-9]{40}$/i', $key);
      } else {
        return !!preg_match('/^[-_a-z0-9]{5,128}$/i', $key);
      }
    }

    public static function Response($http_code, $body, $content_type = null, $key="") {
      $jsonp = self::determineJSONPCallback();
      if ($http_code==404 && $jsonp) {
        # Yuck
        $body = null;
        $http_code = 200;
      }
      if (isset($jsonp)) {
        $content_type = "text/javascript";
        $command = "";
        $body = $_GET[$jsonp] . '(' . json_encode($body) . ',' . json_encode($key) . ');';
      }
      $http_status_messages = array(
        200 => 'OK',
        400 => 'Bad Request',
        404 => 'Not Found',
        413 => 'Request Entity Too Large',
        500 => 'Internal Server Error'
      );
      $http_message = $http_status_messages[$http_code];
      header("HTTP/1.0 {$http_code} {$http_message}");
      header("Content-Type: {$content_type}");
      if(!is_string($body)) {
        $body['documentation_url'] = 'http://openkeyval.org/';
        $body = json_encode($body);
      }
      $content_type = is_null($content_type) ? 'text/plain' : $content_type;
      if (!isset($GLOBALS['WAS_HEAD'])) {
        echo $body;
      }
      exit;
    }

    protected static function Salt() {
      static $salt = null;
      if(is_null($salt)) {
        $filename = $GLOBALS['CONFIG']['dir'] . '/' . 'salt.txt';
        if(!file_exists($filename)) {
          die("Missing salt file (salt.txt in ".$GLOBALS['CONFIG']['dir'].").");
        }
        $salt = trim(file_get_contents($filename));
      }
      return $salt;
    }

    public static function HashForKey($key) {
      return sha1(self::Salt() . $key);
    }

    public static function ReadOnlyKey($key) {
      return OpenKeyval::kReadOnlyKeyPrefix . self::HashForKey($key);
    }
  }

  class OpenKeyval_Storage {
    protected function PathForHash($hash) {
      $dirname = $GLOBALS['CONFIG']['data_dir'] . '/' . substr($hash, 0, 2) . '/'  . substr($hash, 2, 2) . '/'  . substr($hash, 4, 2) . '/'  . substr($hash, 6, 2);

      if(!file_exists($dirname)) {
        mkdir($dirname, 0777, $recursive = true);
      }

      return $dirname . '/' . $hash;
    }


    public static function Delete($key) {
      if(OpenKeyval::IsReadOnlyKey($key) ||
         OpenKeyval::IsWriteOnceKey($key)) {
        //  Can't delete a write-once key or with a read-only key
        return false;
      }

      $hash = OpenKeyval::HashForKey($key);
      $path = self::PathForHash($hash);
      if(!file_exists($path)) {
        return null;
      }
      return unlink($path);
    }

    public static function Get($key) {
      if(OpenKeyval::IsReadOnlyKey($key)) {
        $hash = substr($key, strlen(OpenKeyval::kReadOnlyKeyPrefix));
        $path = self::PathForHash($hash);
      } else {
        $hash = OpenKeyval::HashForKey($key);
        $path = self::PathForHash($hash);
      }
      if(!file_exists($path)) {
        return null;
      }
      return file_get_contents($path);
    }

    public static function Set($key, $value) {
      if(OpenKeyval::IsReadOnlyKey($key)) {
        //  Can't write to a read-only key
        return false;
      }

      $hash = OpenKeyval::HashForKey($key);
      $path = self::PathForHash($hash);
      
      if (OpenKeyval::IsWriteOnceKey($key)) {
        if (!self::CheckAndSetKey($path)) {
          // Write-once key already written to
          return false;
        }
      }
      
      $rv = file_put_contents($path, $value, LOCK_EX);
      return ($rv !== false);
    }
    
    public static function CheckAndSetKey($path) {
      $resource = @fopen($path, 'x+b');
      
      if ($resource === false) {
        // Couldn't create the file, means it already exists
        return false;
      }
      
      fclose($resource);
      return true;
    }
  }

  class OpenKeyval_Storage_Cached extends OpenKeyval_Storage {
    protected static $handle;
    const kCacheVersion = "v1";
    
    public static function Delete($key) {
      if(!GetMemcacheHandle()) {
        //  Memcache down?
        return parent::Delete($key);
      }

      GetMemcacheHandle()->set(self::wrapKeyForMemcache($key),null,0);
      GetMemcacheHandle()->set(self::wrapKeyForMemcache(OpenKeyval::kReadOnlyKeyPrefix.OpenKeyval::HashForKey($key)),null,0);
      return parent::Delete($key);
    }

    public static function Get($key) {
      if(!GetMemcacheHandle()) {
        //  Memcache down?
        return parent::Get($key);
      }

      if($value = GetMemcacheHandle()->get(self::wrapKeyForMemcache($key))) {
        $raw_value = unserialize($value);
        if ($raw_value != null) {
          // Not sure why there are NULLs in the cache, but there are.  TODO: Fix root cause.
          return $raw_value;
        }
      }

      //  not in cache
      $value = parent::Get($key);
      GetMemcacheHandle()->set(self::wrapKeyForMemcache($key), serialize($value));

      return $value;
    }

    public static function Set($key, $value) {
      if(!GetMemcacheHandle()) {
        //  Memcache down?
        return parent::Set($key, $value);
      }

      $hash = OpenKeyval::HashForKey($key);

      $rv = parent::Set($key, $value);
      if($rv) {
        GetMemcacheHandle()->set(self::wrapKeyForMemcache($key), serialize($value));
        GetMemcacheHandle()->set(self::wrapKeyForMemcache(OpenKeyval::kReadOnlyKeyPrefix . $hash), serialize($value));
      } else {
        //  weird, set failed
        GetMemcacheHandle()->set(self::wrapKeyForMemcache($key),null,0);
        GetMemcacheHandle()->set(self::wrapKeyForMemcache(OpenKeyval::kReadOnlyKeyPrefix . $hash),null,0);
      }
      return $rv;
    }
  
    private function wrapKeyForMemcache($key) {
      $namespace = $GLOBALS['CONFIG']['api_hostname'];
      return $namespace . kCacheVersion . "-" . $key;
    }
        
  }

 
  function generateRandStr($length){ 
    $randstr = ""; 
    for($i=0; $i<$length; $i++) { 
       $randnum = mt_rand(0,61); 
       if($randnum < 10) { 
          $randstr .= chr($randnum+48); 
       } else if($randnum < 36) { 
          $randstr .= chr($randnum+55); 
       } else { 
          $randstr .= chr($randnum+61); 
       }
    }
    return $randstr; 
  } 

  function GetMemcacheHandle() {
    static $handle;
    if(!class_exists('Memcache')) {
      return false;
    }
    if(is_null($handle)) {
      $handle = new Memcache();
      $handle->connect('localhost', 11211) or $handle = false;
    }
    return $handle;
  }
    
?>
