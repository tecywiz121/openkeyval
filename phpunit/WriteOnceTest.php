<?php

require_once('../config.inc');
require_once('../api/server.inc');
require_once('curl.class.php');

global $CONFIG;

class WriteOnce extends PHPUnit_Framework_TestCase {
  private static $browser;
  private static $random_key;
  private static $random_value;
  
  public function WriteOnce() {
    self::$random_key = 'wok-' . generateRandStr(rand(5,20));
    self::$random_value = generateRandStr(rand(40,80));
  }
  
  public function Setup() {
    self::$browser = new extractor();
  }
    
  public function testSet() {      
    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/' . self::$random_key;
    $data = self::$browser->getdata($url, array('data'=>self::$random_value));
    $r = json_decode($data);       

    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/' . self::$random_key . "?key_info";
    $data = self::$browser->getdata($url);
    $r = json_decode($data);
    $this->assertEquals($r->is_write_once,true);
  }

  public function testFailAtSettingWOK() {      
    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/'.self::$random_key;
    $data = self::$browser->getdata($url, array('data'=>"this shouldn't work"));
    $r = json_decode($data);
    $this->assertEquals($r->error,"save_failed");    
  }

  public function testGet() {      
    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/'.self::$random_key;
    $data = self::$browser->getdata($url);
    $this->assertEquals($data,self::$random_value);    
  }

  public function testDelete() {      
    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/' . self::$random_key;
    $data = self::$browser->getdata($url, array('data'=>""));
    $r = json_decode($data);
    $this->assertEquals($r->status,"delete_failed");     
       
    $url = (isset($GLOBALS['CONFIG']['ssl'])?"https://":"http://").$GLOBALS['CONFIG']['api_hostname'].'/' . self::$random_key;
    $data = self::$browser->getdata($url);
    $this->assertEquals($data,self::$random_value);
  }



}

?>
