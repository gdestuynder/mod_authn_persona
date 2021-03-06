#!/usr/bin/env perl
#
use strict;
use warnings FATAL => 'all';

use Apache::Test;
use Apache::TestRequest;
use Apache::TestUtil;

plan tests => 4, need_module('JSON');

require JSON;
JSON->import;

my $idp = "https://testidp.org";

my ($domain, $password, $email);

{
  my $res = GET "$idp/api/domain";

  ok $res->is_success;

  my $data = decode_json($res->content);

  $domain = $data->{'domain'};
  $password = $data->{'password'};
  $email = "test\@$domain";

  t_debug("email=$email");

  ok ($domain and $password);
}

{ #Initial request
  my $res = GET "/auth/";
  ok t_cmp( $res->code, 401, "Initial request unauthorided");
}

my $ua = LWP::UserAgent->new;
$ua->default_header('X-Password' => $password);
{
  my $req = HTTP::Request->new("DELETE", "$idp/api/$domain");
  my $res = $ua->request($req);
  ok $res->is_success;
}

