ffrdp ��һ������ udp �Ŀ��ٿɿ�Э��

rto ���㣺
��ʼ��
rtts = rttm
rttd = rttm / 2;

������
rtts = (1 - alpha) * rtts + alpha * rttm; // alpha = 1 / 8
rttd = (1 - beta ) * rttd + beta  *(rttm - rtts); // beta = 1 / 4

������
rto  = rtts + r * rttd;
��ʱ��
rto  = 1.5 * rto

֡���壺
data frame: 0x00 seq0 seq1 seq2 data ...
ack  frame: 0x01 una0 una1 una2 mack0 mack1 wind0 wind1
win0 frame: 0x02
win1 frame: 0x03 wind0 wind1
bye  frame: 0x04

Э���ص㣺
ѡ�����ش��������ش������ӳ� ACK��UNA + MACK������������


